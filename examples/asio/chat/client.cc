#include "codec.h"

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/TcpClient.h>

#include <boost/bind.hpp>
#include <boost/noncopyable.hpp>

#include <iostream>
#include <stdio.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

//注：ChatClient包括两大部分：1、TCPClient，用于进行网络通讯。2、LengthHeaderCodec，用于解析报文
//整个ChatClient对象都运行在子线程中

//底层的网络通讯是共享的，无论父线程还是子线程，只要有相应的socket fd，就能进行网络通讯

//还需要注意的是多线程和对象之间的关系，线程是底层机制，而对象是上层的概念，他们是完全解耦的。所以多个线程完全可以对多个线程
//进行操作，本例即是如此，ChatClient对象只有一个，但是却有两个线程对其进行操作。两个线程怎样定位到统一个对象呢？通过回调函数
//传递的的this指针。

//client的实现思路：
/*
	两个线程，主线程用于：a、发数据，包括发起连接，向Server发送字符串（但最终发送还是在子线程中的loop中完成，见TcpConnection.cc，这里运用了回调函数）。
	b、阻塞读取标准输入
	子线程：a、接收数据：包括接收连接建立完成的报文，接收Server发送过来的消息。
*/
class ChatClient : boost::noncopyable
{
 public:
  ChatClient(EventLoop* loop, const InetAddress& serverAddr)
    : client_(loop, serverAddr, "ChatClient"),
      codec_(boost::bind(&ChatClient::onStringMessage, this, _1, _2, _3))//codec_的构造必须传入回调函数，此回调函数在报文解析完成以后调用
  {
    client_.setConnectionCallback(
        boost::bind(&ChatClient::onConnection, this, _1)); //连接建立成功后调用
    client_.setMessageCallback(
        boost::bind(&LengthHeaderCodec::onMessage, &codec_, _1, _2, _3));//从Server端收到一个数据报头后进行调用，解析报头
    client_.enableRetry();
  }

  void connect()
  {
    client_.connect();
  }

  void disconnect()
  {
    client_.disconnect();
  }

  void write(const StringPiece& message)
  {
    MutexLockGuard lock(mutex_);
    if (connection_)
    {
      codec_.send(get_pointer(connection_), message);
    }
  }

 private:
  void onConnection(const TcpConnectionPtr& conn)
  {
    LOG_INFO << conn->localAddress().toIpPort() << " -> "
             << conn->peerAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");

    MutexLockGuard lock(mutex_);
    if (conn->connected())
    {
      connection_ = conn;
    }
    else
    {
      connection_.reset();
    }
  }

  void onStringMessage(const TcpConnectionPtr&,
                       const string& message,
                       Timestamp)
  {
    printf("<<< %s\n", message.c_str());
  }

  TcpClient client_;
  LengthHeaderCodec codec_;
  MutexLock mutex_;
  TcpConnectionPtr connection_;
};

int main(int argc, char* argv[])
{
  LOG_INFO << "pid = " << getpid();
  if (argc > 2)
  {
    EventLoopThread loopThread;
    uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
    InetAddress serverAddr(argv[1], port);

	//下面两行代码很精妙。新建一个线程专门运行ChatClient，在ChatClient注册了连接成功和收到消息的回调函数
	//连接的发起是在主线程的client.connect()发起，但连接建立成功会调用子线程中的回调函数。主线程在发起连接
	//后就进入监控stdin的循环，由此可见muduo的connect是非阻塞的。事实证明的确如此，见代码Connector.cc
    ChatClient client(loopThread.startLoop(), serverAddr);
    client.connect();
    std::string line;
    while (std::getline(std::cin, line))
    {
      client.write(line);
    }
    client.disconnect();
    CurrentThread::sleepUsec(1000*1000);  // wait for disconnect, see ace/logging/client.cc
  }
  else
  {
    printf("Usage: %s host_ip port\n", argv[0]);
  }
}

