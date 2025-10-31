#include "../../common/tick.hpp"
#include "../../common/timing.hpp"
#include <hiredis/hiredis.h>
#include <iostream>
#include <random>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

using namespace qtick;

std::atomic<bool> running{true};

void signal_handler(int) {
    running.store(false);
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    
    // Connect to Redis via UNIX socket
    const char* socket_path = "/var/run/redis/redis.sock";
    if (argc > 1) {
        socket_path = argv[1];
    }
    
    std::cout << "Connecting to Redis at: " << socket_path << "\n";
    
    redisContext* ctx = redisConnectUnix(socket_path);
    if (ctx == nullptr || ctx->err) {
        std::cerr << "Redis connection error: " 
                  << (ctx ? ctx->errstr : "allocation failed") << "\n";
        return 1;
    }
    
    std::cout << "Connected. Generating ticks...\n";
    
    // Random price generator
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> sym_dist(0, static_cast<int>(Symbol::COUNT) - 1);
    std::uniform_real_distribution<> price_dist(100.0, 500.0);
    std::uniform_int_distribution<> size_dist(100, 10000);
    
    uint64_t seq = 0;
    uint64_t total_sent = 0;
    
    const int ticks_per_sec = 10000; // target rate
    const auto sleep_interval = std::chrono::microseconds(1'000'000 / ticks_per_sec);
    
    auto last_report = std::chrono::steady_clock::now();
    
    while (running.load()) {
        // Generate tick
        Tick tick(
            get_timestamp_ns(),
            static_cast<Symbol>(sym_dist(gen)),
            price_dist(gen),
            size_dist(gen),
            seq++
        );
        
        // Publish to Redis channel as binary
        redisReply* reply = (redisReply*)redisCommand(
            ctx, 
            "PUBLISH ticks %b",
            &tick, sizeof(Tick)
        );
        
        if (reply) {
            freeReplyObject(reply);
            ++total_sent;
        } else {
            std::cerr << "Publish failed\n";
            break;
        }
        
        // Rate limiting
        std::this_thread::sleep_for(sleep_interval);
        
        // Report stats every second
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 1) {
            std::cout << "Sent: " << total_sent << " ticks\n";
            last_report = now;
        }
    }
    
    std::cout << "\nShutting down. Total sent: " << total_sent << "\n";
    redisFree(ctx);
    return 0;
}