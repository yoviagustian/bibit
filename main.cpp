#include <array>
#include <iostream>
#include <memory>
#include<string>
#include <cstring>

#include <asio.hpp>
#include <sw/redis++/redis++.h>

// Write Filesystem
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <time.h>

// Disruptor
#include "Disruptor/Disruptor.h"
#include "Disruptor/ThreadPerTaskScheduler.h"
#include <thread>

using asio::buffer;
using asio::ip::tcp;
using namespace sw::redis;

int GetFileDescriptor(std::filebuf& filebuf)
{
  class my_filebuf : public std::filebuf
  {
  public:
    int handle() { return _M_file.fd(); }
  };

  return static_cast<my_filebuf&>(filebuf).handle();
}

void logToFile(std::string fileName, std::string data){
  std::ofstream out;

  time_t now = time(&now);
  struct tm *ptm = gmtime(&now);
  fileName = fileName + " " + asctime(ptm);

  out.open(fileName, std::ios_base::app);
  out << data;
  out.flush();

  fsync(GetFileDescriptor(*out.rdbuf()));
}

// Multi Thread Disruptor
struct Event
{
    std::string fileName;
    std::string data;
};

struct EventHandler : Disruptor::IEventHandler< Event >
{
    explicit EventHandler(int toProcess) : m_toProcess(toProcess),  m_actuallyProcessed(0)
    {}

    void onEvent(Event& event, int64_t, bool) override
    {
        logToFile(event.fileName, event.data);
        m_allDone.notify_all();
    }

    void waitEndOfProcessing()
    {
        std::unique_lock<decltype(m_mutex)> lk(m_mutex);
        m_allDone.wait(lk);
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_allDone;
    int m_toProcess;
    int m_actuallyProcessed;
};

auto const ExpectedNumberOfEvents = 10;
auto const RingBufferSize = 1024;

// Instantiate and start the disruptor
auto eventFactory = []() { return Event(); };
auto taskScheduler = std::make_shared< Disruptor::ThreadPerTaskScheduler >();
    
auto disruptor = std::make_shared< Disruptor::disruptor<Event> >(eventFactory, RingBufferSize, taskScheduler);
auto printingEventHandler = std::make_shared< EventHandler >(ExpectedNumberOfEvents);


void redisSet(std::string key, int byteSize) {
  try
    {
        auto redis = Redis("tcp://127.0.0.1:6379");

        auto val = redis.get(key);
        if (val)
        {
          int sum = byteSize + stoi(*val);
          redis.set(key, std::to_string(sum));;
        }else{
          redis.set(key, std::to_string(byteSize));;
        }
    }
    catch (const Error &e)
    {
        std::cout << "Error Redis" << std::endl;
    }
}

class proxy
  : public std::enable_shared_from_this<proxy>
{
public:
  proxy(tcp::socket client)
    : client_(std::move(client)),
      server_(client_.get_executor())
  {
  }

  void connect_to_server(tcp::endpoint target, auto &disruptor)
  {
    auto self = shared_from_this();
    server_.async_connect(
        target,
        [self, &disruptor](std::error_code error)
        {
          if (!error)
          {
            self->read_from_client(disruptor);
            self->read_from_server(disruptor);
          }
        }
      );
  }

private:
  void stop()
  {
    client_.close();
    server_.close();
  }

  void read_from_client(auto &disruptor)
  {
    auto self = shared_from_this();
    client_.async_read_some(
        buffer(data_from_client_),
        [self, &disruptor](std::error_code error, std::size_t n)
        {
          if (!error)
          {
            self->write_to_server(n, disruptor);

            // Count Bytes to Redis 
            std::string clientIP = self->client_.remote_endpoint().address().to_string();
            redisSet(clientIP, n);

            // Log Raw Data
            std::string data(self->data_from_client_.begin(), self->data_from_client_.end());

            // Publish events
            auto ringBuffer = disruptor->ringBuffer();
            auto nextSequence = ringBuffer->next();
            (*ringBuffer)[nextSequence].fileName = clientIP;
            (*ringBuffer)[nextSequence].data = data;

            ringBuffer->publish(nextSequence);

            printingEventHandler->waitEndOfProcessing();
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  void write_to_server(std::size_t n, auto &disruptor)
  {
    auto self = shared_from_this();
    async_write(
        server_,
        buffer(data_from_client_, n),
        [self, &disruptor](std::error_code ec, std::size_t /*n*/)
        {
          if (!ec)
          {
            self->read_from_client(disruptor);
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  void read_from_server(auto &disruptor)
  {
    auto self = shared_from_this();
    server_.async_read_some(
        asio::buffer(data_from_server_),
        [self, &disruptor](std::error_code error, std::size_t n)
        {
          if (!error)
          {
            self->write_to_client(n, disruptor);

            std::string clientIP = self->client_.remote_endpoint().address().to_string();
            std::string data(self->data_from_server_.begin(), self->data_from_server_.end());
            // logToFile(clientIP, data);

            // Publish events
            auto ringBuffer = disruptor->ringBuffer();
            auto nextSequence = ringBuffer->next();
            (*ringBuffer)[nextSequence].fileName = clientIP;
            (*ringBuffer)[nextSequence].data = data;

            ringBuffer->publish(nextSequence);

            printingEventHandler->waitEndOfProcessing();
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  void write_to_client(std::size_t n, auto &disruptor)
  {
    auto self = shared_from_this();
    async_write(
        client_,
        buffer(data_from_server_, n),
        [self, &disruptor](std::error_code ec, std::size_t /*n*/)
        {
          if (!ec)
          {
            self->read_from_server(disruptor);
          }
          else
          {
            self->stop();
          }
        }
      );
  }

  tcp::socket client_;
  tcp::socket server_;
  std::array<char, 1024> data_from_client_;
  std::array<char, 1024> data_from_server_;
};

void listen(tcp::acceptor& acceptor, tcp::endpoint target, auto &disruptor)
{
  acceptor.async_accept(
      [&acceptor, target, &disruptor](std::error_code error, tcp::socket client)
      {
        if (!error)
        {
          std::make_shared<proxy>(
              std::move(client)
            )->connect_to_server(target, disruptor);
        }

        listen(acceptor, target, disruptor);
      }
    );
}

int main()
{
    disruptor->handleEventsWith(printingEventHandler);

    taskScheduler->start();
    disruptor->start();

  try
  {
    std::string aceptor = "";
    std::string port1 = "54545";
    std::string target = "localhost";
    std::string port2 = "80";

    asio::io_context ctx;

    auto listen_endpoint =
      *tcp::resolver(ctx).resolve(
          aceptor,
          port1,
          tcp::resolver::passive
        );

    auto target_endpoint =
      *tcp::resolver(ctx).resolve(
          target,
          port2
        );

    tcp::acceptor acceptor(ctx, listen_endpoint);

    listen(acceptor, target_endpoint, disruptor);

    ctx.run();

    // Wait for the end of execution and shutdown
    printingEventHandler->waitEndOfProcessing();

    disruptor->shutdown();
    taskScheduler->stop();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }
}