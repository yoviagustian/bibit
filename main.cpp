#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <cstring>

#include <asio.hpp>
#include <sw/redis++/redis++.h>

// Write Filesystem
#include <fstream>
#include <unistd.h>
#include <time.h>

// Disruptor
#include "Disruptor/Disruptor.h"
#include "Disruptor/ThreadPerTaskScheduler.h"
#include <thread>

using asio::buffer;
using asio::ip::tcp;
using namespace sw::redis;

// Instantiate the redis
auto redis = Redis("tcp://127.0.0.1:6379");

void redisSet(std::string key, long long byteSize)
{
  try
  {
    redis.incrby(key, byteSize);
  }
  catch (const Error &e)
  {
    std::cout << "Error Redis" << std::endl;
  }
}

// Instantiate the logfile
std::ofstream out;

int GetFileDescriptor(std::filebuf &filebuf)
{
  class my_filebuf : public std::filebuf
  {
  public:
    int handle() { return _M_file.fd(); }
  };

  return static_cast<my_filebuf &>(filebuf).handle();
}

void logToFile(std::string ip, std::string data)
{

  time_t now = time(&now);
  struct tm *ptm = gmtime(&now);
  ip = "\n>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> " + ip + " " + asctime(ptm) + "\n";

  out << data;
  out << ip;
  out.flush();

  fsync(GetFileDescriptor(*out.rdbuf()));
}

// Instantiate the disruptor
struct Event
{
  std::string fileName;
  std::string data;
};

struct EventHandler : Disruptor::IEventHandler<Event>
{
  explicit EventHandler() : m_toProcess(), m_actuallyProcessed(0)
  {
  }

  void onEvent(Event &event, int64_t, bool) override
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

auto const RingBufferSize = 1024;
auto eventFactory = [](){ return Event(); };
auto taskScheduler = std::make_shared<Disruptor::ThreadPerTaskScheduler>();

auto disruptor = std::make_shared<Disruptor::disruptor<Event>>(eventFactory, RingBufferSize, taskScheduler);
auto eventHandler = std::make_shared<EventHandler>();

// Asio
class proxy
    : public std::enable_shared_from_this<proxy>
{
public:
  proxy(tcp::socket client)
      : client_(std::move(client)),
        server_(client_.get_executor())
  {
  }

  void connect_to_server(tcp::endpoint target)
  {
    auto self = shared_from_this();
    server_.async_connect(
        target,
        [self](std::error_code error)
        {
          if (!error)
          {
            self->read_from_client();
            self->read_from_server();
          }
        });
  }

private:
  void stop()
  {
    client_.close();
    server_.close();
  }

  void read_from_client()
  {
    auto self = shared_from_this();
    client_.async_read_some(
        buffer(data_from_client_),
        [self](std::error_code error, std::size_t n)
        {
          if (!error)
          {
            self->write_to_server(n);

            // Count Bytes to Redis
            std::string clientIP = self->client_.remote_endpoint().address().to_string();
            redisSet(clientIP, n);

            // Raw Data
            std::string data(self->data_from_client_.begin(), self->data_from_client_.end());

            // Publish events
            auto ringBuffer = disruptor->ringBuffer();
            auto nextSequence = ringBuffer->next();
            (*ringBuffer)[nextSequence].fileName = "[IN] " + clientIP;
            (*ringBuffer)[nextSequence].data = data;

            ringBuffer->publish(nextSequence);

            eventHandler->waitEndOfProcessing();
          }
          else
          {
            self->stop();
          }
        });
  }

  void write_to_server(std::size_t n)
  {
    auto self = shared_from_this();
    async_write(
        server_,
        buffer(data_from_client_, n),
        [self](std::error_code ec, std::size_t /*n*/)
        {
          if (!ec)
          {
            self->read_from_client();
          }
          else
          {
            self->stop();
          }
        });
  }

  void read_from_server()
  {
    auto self = shared_from_this();
    server_.async_read_some(
        asio::buffer(data_from_server_),
        [self](std::error_code error, std::size_t n)
        {
          if (!error)
          {
            self->write_to_client(n);

            std::string clientIP = self->client_.remote_endpoint().address().to_string();
            std::string data(self->data_from_server_.begin(), self->data_from_server_.end());

            // Publish events
            auto ringBuffer = disruptor->ringBuffer();
            auto nextSequence = ringBuffer->next();
            (*ringBuffer)[nextSequence].fileName = "[OUT] " + clientIP;
            (*ringBuffer)[nextSequence].data = data;

            ringBuffer->publish(nextSequence);

            eventHandler->waitEndOfProcessing();
          }
          else
          {
            self->stop();
          }
        });
  }

  void write_to_client(std::size_t n)
  {
    auto self = shared_from_this();
    async_write(
        client_,
        buffer(data_from_server_, n),
        [self](std::error_code ec, std::size_t /*n*/)
        {
          if (!ec)
          {
            self->read_from_server();
          }
          else
          {
            self->stop();
          }
        });
  }

  tcp::socket client_;
  tcp::socket server_;
  std::array<char, 1024> data_from_client_;
  std::array<char, 1024> data_from_server_;
};

void listen(tcp::acceptor &acceptor, tcp::endpoint target)
{
  acceptor.async_accept(
      [&acceptor, target](std::error_code error, tcp::socket client)
      {
        if (!error)
        {
          std::make_shared<proxy>(
              std::move(client))
              ->connect_to_server(target);
        }

        listen(acceptor, target);
      });
}

int main()
{
  try
  { 
    // Start Disruptor
    disruptor->handleEventsWith(eventHandler);
    taskScheduler->start();
    disruptor->start();

    // Open Log File
    out.open("Log", std::ios_base::app);

    std::string aceptor = "";
    std::string aceptorPort = "54545";
    std::string target = "localhost";
    std::string targetPort = "80";

    asio::io_context ctx;

    auto listen_endpoint =
        *tcp::resolver(ctx).resolve(
            aceptor,
            aceptorPort,
            tcp::resolver::passive);

    auto target_endpoint =
        *tcp::resolver(ctx).resolve(
            target,
            targetPort);

    tcp::acceptor acceptor(ctx, listen_endpoint);

    // Start ASIO
    listen(acceptor, target_endpoint);

    ctx.run();

    // Wait for the end of execution and shutdown disruptor
    eventHandler->waitEndOfProcessing();
    disruptor->shutdown();
    taskScheduler->stop();

    // Close Log File
    out.close();
  }
  catch (std::exception &e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }
}