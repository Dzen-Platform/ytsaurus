#pragma once

#include <util/generic/ptr.h>
#include <util/stream/input.h>
#include <util/stream/output.h>
#include <util/thread/queue.h>

#include <functional>

class TInetStreamSocket;

// Simple server listens on the specified port and launches
// requestHandler in the separate thread for each incoming connection.
class TSimpleServer {
public:
    using TRequestHandler = std::function<void(IInputStream* input, IOutputStream* output)>;

public:
    TSimpleServer(int port, TRequestHandler requestHandler);
    ~TSimpleServer();

    void Stop();

    int GetPort() const;

private:
    const int Port;
    THolder<IMtpQueue> ThreadPool;
    THolder<IThreadPool::IThread> ListenerThread;
    THolder<TInetStreamSocket> SendFinishSocket;
};
