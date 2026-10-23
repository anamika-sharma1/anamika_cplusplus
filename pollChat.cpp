/ poll_queue_demo.cpp - a tiny, self-contained demo. Just build and run it.
//
//   main thread   : the server's I/O loop. Uses poll() to tell sockets apart:
//                     listenFd ready -> accept()   (a new client)
//                     client fd ready -> recv()    (a client sent data)
//                   Complete lines are pushed into a queue (producer).
//   worker thread : pops lines from the queue and processes them (consumer).
//   3 client threads : fake users that connect and send messages, so the
//                      demo runs on its own without nc or a second terminal.
//
// Build: g++ -std=c++17 -pthread poll_queue_demo.cpp -o poll_queue_demo
// Run:   ./poll_queue_demo

// Inspiration drawn from real prod servers using ngnix, redis or node.js in which we use use one thread, or a few, for thousands of clients.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
 
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
 
const int PORT = 5558;
 
std::mutex printMtx;   // several threads print; keep lines whole
void log(const std::string& who, const std::string& text) {
    std::lock_guard<std::mutex> lock(printMtx);
    std::cout << "[" << who << "] " << text << std::endl;
}
 
// ---------------- the queue: I/O thread -> worker ----------------
struct Message { int fd; std::string line; };
std::queue<Message> work;
std::mutex workMtx;
std::condition_variable workCv;
bool done = false;
 
void worker() {
    while (true) {
        std::unique_lock<std::mutex> lock(workMtx);
        workCv.wait(lock, [] { return !work.empty() || done; });
        if (work.empty()) return;                   // done and nothing left
        Message m = work.front();
        work.pop();
        lock.unlock();                              // process WITHOUT holding the lock
        log("worker", "processing fd " + std::to_string(m.fd) + ": \"" + m.line + "\"");
    }
}
 
// ---------------- fake clients ----------------
void client(int id, std::vector<std::string> pieces, int startDelayMs) {
    usleep(startDelayMs * 1000);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    for (const auto& p : pieces) {
        log("client " + std::to_string(id), "sends \"" + p.substr(0, p.find('\n')) +
                                             (p.back() == '\n' ? "\\n\"" : "\" (no newline yet)"));
        send(fd, p.data(), p.size(), 0);
        usleep(300 * 1000);
    }
    close(fd);
}
 
// ---------------- main = the server's I/O thread ----------------
int main() {
    // setup: socket, bind, listen
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listenFd, SOMAXCONN) < 0) {
        std::cerr << "bind/listen failed\n";
        return 1;
    }
    log("server", "listening, listenFd = " + std::to_string(listenFd));
 
    std::thread w(worker);
    std::thread c1(client, 1, std::vector<std::string>{"hello\n", "bye\n"}, 100);
    std::thread c2(client, 2, std::vector<std::string>{"hel", "lo from 2\n"}, 250);
    std::thread c3(client, 3, std::vector<std::string>{"hi, I joined last\n"}, 400);
 
    std::vector<pollfd> fds;
    fds.push_back({listenFd, POLLIN, 0});           // fds[0] is ALWAYS the listening socket
    std::map<int, std::string> buffers;             // per-client partial lines
    int finished = 0;
 
    while (finished < 3) {                          // stop after all 3 fake clients leave
        poll(fds.data(), fds.size(), -1);           // sleep until ANY socket is ready
 
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].revents == 0) continue;      // this one isn't ready
            int fd = fds[i].fd;
 
            if (fd == listenFd) {
                // ---- the listening socket is ready: a new client is waiting ----
                int clientFd = accept(listenFd, nullptr, nullptr);
                fds.push_back({clientFd, POLLIN, 0});   // watch it from now on
                log("server", "listenFd ready -> accept() -> new client fd " + std::to_string(clientFd));
                continue;
            }
 
            // ---- a client socket is ready: it sent data (or closed) ----
            char chunk[1024];
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);   // ONE recv per wake-up
            if (n <= 0) {
                log("server", "fd " + std::to_string(fd) + " ready -> recv() = 0 -> client left");
                close(fd);
                buffers.erase(fd);
                fds.erase(fds.begin() + i);
                --i;                                // next element moved into slot i
                ++finished;
                continue;
            }
 
            std::string& buf = buffers[fd];
            buf.append(chunk, n);
            log("server", "fd " + std::to_string(fd) + " ready -> recv() " + std::to_string(n) + " bytes");
            std::size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                {
                    std::lock_guard<std::mutex> lock(workMtx);
                    work.push({fd, line});          // PRODUCER: hand the line to the worker
                }
                workCv.notify_one();
                log("server", "  complete line queued for worker");
            }
        }
    }
 
    // shutdown: tell the worker to finish the queue and stop
    { std::lock_guard<std::mutex> lock(workMtx); done = true; }
    workCv.notify_one();
    c1.join(); c2.join(); c3.join(); w.join();
    close(listenFd);
    log("server", "all clients gone, worker finished, bye");
}
