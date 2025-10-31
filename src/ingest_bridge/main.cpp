#include "../../common/tick.hpp"
#include "../../common/ring.hpp"
#include "../../common/timing.hpp"
#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>

extern "C" {
#include "k.h"  // kdb+ C API
}

using namespace qtick;

constexpr size_t RING_SIZE = 4096;  // power of 2
constexpr size_t BATCH_SIZE = 64;

std::atomic<bool> running{true};
SPSCRing<Tick, RING_SIZE> ring;

void signal_handler(int) {
    running.store(false);
}

void init_logging() {
    spdlog::init_thread_pool(8192, 1);
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "qtick", console_sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block
    );
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

// Thread A: Redis subscriber - reads ticks and pushes to ring
void reader_thread(const char* socket_path) {
    spdlog::info("Reader thread starting, connecting to {}", socket_path);
    
    redisContext* ctx = redisConnectUnix(socket_path);
    if (ctx == nullptr || ctx->err) {
        spdlog::error("Redis connection failed: {}", ctx ? ctx->errstr : "null");
        running.store(false);
        return;
    }
    
    // Subscribe to ticks channel
    redisReply* reply = (redisReply*)redisCommand(ctx, "SUBSCRIBE ticks");
    if (!reply) {
        spdlog::error("Subscribe failed");
        redisFree(ctx);
        running.store(false);
        return;
    }
    freeReplyObject(reply);
    
    uint64_t received = 0;
    uint64_t drops = 0;
    
    spdlog::info("Subscribed to 'ticks' channel");
    
    while (running.load()) {
        redisReply* msg = nullptr;
        if (redisGetReply(ctx, (void**)&msg) == REDIS_OK && msg) {
            // Message format: array [type, channel, data]
            if (msg->type == REDIS_REPLY_ARRAY && msg->elements == 3) {
                redisReply* data = msg->element[2];
                if (data->type == REDIS_REPLY_STRING && 
                    data->len == sizeof(Tick)) {
                    
                    Tick tick;
                    std::memcpy(&tick, data->str, sizeof(Tick));
                    
                    // Mark receive time
                    uint64_t t1 = get_timestamp_ns();
                    
                    // Push to ring
                    if (!ring.try_push(tick)) {
                        ++drops;
                        if (drops % 1000 == 0) {
                            spdlog::warn("Ring full, drops: {}", drops);
                        }
                    } else {
                        ++received;
                    }
                }
            }
            freeReplyObject(msg);
        } else {
            break;
        }
        
        // Periodic stats
        if (received % 10000 == 0 && received > 0) {
            spdlog::info("Received: {} | Drops: {} | Ring: {}/{}", 
                        received, drops, ring.approx_size(), RING_SIZE);
        }
    }
    
    spdlog::info("Reader thread exiting. Total received: {}, drops: {}", 
                received, drops);
    redisFree(ctx);
}

// Send batch to kdb+ via IPC
bool send_batch_to_q(int handle, const TickBatch<BATCH_SIZE>& batch) {
    if (batch.is_empty()) return true;
    
    const size_t n = batch.size();
    
    // Build K arrays for vectorized insert
    // upd[`trades; (times; syms; prices; sizes)]
    
    K times = ktn(KP, n);   // timestamp vector (KP = nanosecond timespan)
    K syms = ktn(KS, n);    // symbol vector
    K prices = ktn(KF, n);  // float vector
    K sizes = ktn(KI, n);   // int vector
    
    for (size_t i = 0; i < n; ++i) {
        const Tick& t = batch.ticks[i];
        kP(times)[i] = t.ts_ns;
        kS(syms)[i] = ss((char*)symbol_to_str(static_cast<Symbol>(t.sym_idx)));
        kF(prices)[i] = t.price;
        kI(sizes)[i] = t.size;
    }
    
    // Call: upd[times; syms; prices; sizes]
    K result = k(handle, "upd", times, syms, prices, sizes, (K)0);
    
    if (!result || result->t == -128) { // error
        spdlog::error("q upd failed: {}", result ? result->s : "null");
        if (result) r0(result);
        return false;
    }
    
    r0(result);
    return true;
}

// Thread B: Consumer - pops from ring, batches, sends to q
void writer_thread(const char* q_host, int q_port) {
    spdlog::info("Writer thread starting, connecting to q at {}:{}", q_host, q_port);
    
    // Connect to kdb+
    int q_handle = khpu(const_cast<char*>(q_host), q_port, "");
    if (q_handle <= 0) {
        spdlog::error("Failed to connect to kdb+ at {}:{}", q_host, q_port);
        running.store(false);
        return;
    }
    
    spdlog::info("Connected to kdb+");
    
    TickBatch<BATCH_SIZE> batch;
    uint64_t batches_sent = 0;
    uint64_t ticks_sent = 0;
    
    auto last_flush = std::chrono::steady_clock::now();
    const auto flush_timeout = std::chrono::microseconds(100); // 100us timeout
    
    while (running.load()) {
        Tick tick;
        
        if (ring.try_pop(tick)) {
            batch.push(tick);
            
            // Send when batch full
            if (batch.is_full()) {
                uint64_t t3 = get_timestamp_ns();
                
                if (send_batch_to_q(q_handle, batch)) {
                    uint64_t t4 = get_timestamp_ns();
                    
                    ++batches_sent;
                    ticks_sent += batch.size();
                    
                    if (batches_sent % 100 == 0) {
                        spdlog::info("Batches: {} | Ticks: {} | Last batch latency: {:.2f} us",
                                    batches_sent, ticks_sent, ns_to_us(t4 - t3));
                    }
                }
                
                batch.clear();
                last_flush = std::chrono::steady_clock::now();
            }
        } else {
            // Ring empty - check for timeout flush
            auto now = std::chrono::steady_clock::now();
            if (!batch.is_empty() && 
                (now - last_flush) > flush_timeout) {
                
                if (send_batch_to_q(q_handle, batch)) {
                    ++batches_sent;
                    ticks_sent += batch.size();
                }
                batch.clear();
                last_flush = now;
            }
            
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
    
    // Final flush
    if (!batch.is_empty()) {
        send_batch_to_q(q_handle, batch);
        ticks_sent += batch.size();
    }
    
    spdlog::info("Writer thread exiting. Batches: {}, ticks: {}", 
                batches_sent, ticks_sent);
    kclose(q_handle);
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    init_logging();
    
    const char* redis_socket = "/var/run/redis/redis.sock";
    const char* q_host = "localhost";
    int q_port = 5010;
    
    if (argc > 1) redis_socket = argv[1];
    if (argc > 2) q_host = argv[2];
    if (argc > 3) q_port = std::atoi(argv[3]);
    
    spdlog::info("Starting qtick bridge");
    spdlog::info("Redis: {}", redis_socket);
    spdlog::info("kdb+: {}:{}", q_host, q_port);
    spdlog::info("Ring size: {}, Batch size: {}", RING_SIZE, BATCH_SIZE);
    
    std::thread reader(reader_thread, redis_socket);
    std::thread writer(writer_thread, q_host, q_port);
    
    reader.join();
    writer.join();
    
    spdlog::info("Shutdown complete");
    return 0;
}