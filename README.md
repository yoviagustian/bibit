1. Install Redis-Plus-Plus Library

git clone https://github.com/redis/hiredis.git\
cd hiredis\
make\
make install\
cd ..

git clone https://github.com/sewenew/redis-plus-plus.git\
cd redis-plus-plus\
mkdir build\
cd build\
cmake -DREDIS_PLUS_PLUS_CXX_STANDARD=17 ..\
make\
make install\
cd ..

2. Install  Disruptor

sudo apt-get install libboost-all-dev

git clone https://github.com/Abc-Arbitrage/Disruptor-cpp.git\
cd Disruptor-cpp
mkdir build && cd build\
cmake .. -DCMAKE_BUILD_TYPE=release\
make\
make install\
cd ..

3. Build main.cpp

g++ -std=c++17 main.cpp -o main /usr/local/lib/libredis++.a /usr/local/lib/libhiredis.a /usr/local/lib/libDisruptor.a -lpthread -lboost_system -lboost_thread -fconcepts-ts -pthread -I${workspaceFolder}asio/include -g -DASIO_ENABLE_HANDLER_TRACKING

*** Note ***
+ Run Nginx (localhost:80)
+ Start Redis (localhost:6379)

4. Run program  (-- Running on localhost:54545 --)\
./main
