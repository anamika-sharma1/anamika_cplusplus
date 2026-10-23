# anamika_cplusplus
c++

char* argv[] means “an array of pointers to characters,”
argv ──► [0] ──► "./chat_server\0"
         [1] ──► "6000\0"
         [2] ──► "test.db\0"
         [3] ──► nullptr

Each element argv[i] is a char* pointing to the first character of one null-terminated string (text ending in the special character '\0').

argv[0] is the name of the program itself, as it was typed to run it. That’s why argc is always at least 1

./chat_server 6000 test.db, here argc value is 3
-----------------------------------------------------------------------------------------------------------------------------------------
struct sigaction sa{};          // a settings form, all fields zeroed
sa.sa_handler = onSignal;       // "when the signal arrives, call this function"
sigemptyset(&sa.sa_mask);       // don't block other signals while onSignal runs
sigaction(SIGINT, &sa, nullptr);   // apply these settings to Ctrl+C
sigaction(SIGTERM, &sa, nullptr);  // and to `kill <pid>`

Think of sa as a form you fill in, and sigaction(...) as handing it to the OS. The nullptr means “I don’t need a copy of the old settings back.”

The C++ keyword struct is needed in struct sigaction sa because there is both a struct named sigaction and a function named sigaction. The keyword tells the compiler you mean the struct.

What happens without it. You press Ctrl+C, and the kernel ends the process on the spot, in the middle of whatever any thread was doing. To be fair, the OS still cleans up a lot: it frees memory and closes every socket and file. So nothing explodes. But you lose:
1)Clients get no explanation. Their connections just drop. They never see “Server is shutting down.”
2)Half-finished work is cut off. A thread could be halfway through a broadcast (some users got the message, others didn’t), or in the middle of registering a user.
3)No final bookkeeping. The LEAVE alice lines are never written to the log, and destructors like sqlite3_close() never run.
-----------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }   // only flip the flag

std::queue<int> work;            // shared between the two threads
std::mutex mtx;
std::condition_variable cv;
bool producerDone = false;       // protected by mtx

void producer(int& made) {
    while (g_running) {          // every thread checks the same flag
        {
            std::lock_guard<std::mutex> lock(mtx);
            work.push(++made);
        }
        cv.notify_one();
        usleep(50000);           // a new item every 50 ms
    }
    {
        std::lock_guard<std::mutex> lock(mtx);
        producerDone = true;     // tell the writer: nothing more is coming
    }
    cv.notify_one();
}

void writer(int& saved) {
    std::ofstream out("output.txt");
    while (true) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [] { return !work.empty() || producerDone; });
        if (work.empty()) break;             // producer finished AND queue drained
        int item = work.front();
        work.pop();
        lock.unlock();

        out << "item " << item << '\n';
        ++saved;
        usleep(150000);          // saving is slow: 150 ms per item
    }
}

int main() {
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int made = 0, saved = 0;
    std::thread t1(producer, std::ref(made));
    std::thread t2(writer, std::ref(saved));

    std::cout << "Working... press Ctrl+C to stop\n";
    t1.join();                   // returns once Ctrl+C stops the producer
    std::cout << "Ctrl+C received: producer stopped, writer finishing the queue...\n";
    t2.join();                   // returns once every queued item is saved

    std::cout << "Made " << made << " items, saved " << saved << " to output.txt\n";
    return 0;
}

This program has two threads sharing a queue. A producer makes a work item every 50 ms and puts it in the queue. A writer takes items out and saves them to output.txt, but it’s slower, at 150 ms per item. So at any moment, many items are sitting in the queue in memory, waiting to be saved.

Two separate kinds of in-memory data died with the process:

1) The queue. In 2 seconds the producer made 40 items, but the writer had only handled about 13. The other 27 lived only in std::queue, in RAM. Killing the process erased them.
2) The file buffer. Even the 13 items the writer had written weren’t on disk yet. std::ofstream collects writes in a memory buffer and sends them to disk in big chunks, for speed. The buffer is flushed when the stream is closed by its destructor, and an instant kill means destructors never run. So the file stayed empty.
-----------------------------------------------------------------------------------------------------------------------------------------
When a process ends, whether by a normal return, a crash, or a kill, the OS takes back the whole warehouse at once. It doesn’t look at individual objects. It discards the process’s entire address space, so every byte, leaked or not, is reclaimed instantly. No delete runs, and no destructors run.

So when do leaks actually matter?

Leaks matter while the program is running, not after it exits.
-----------------------------------------------------------------------------------------------------------------------------------------
TCP connection, a pipe of bytes.
