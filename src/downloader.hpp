#pragma once
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "raylib.h"
#include "httplib.h"
#include "raii.hpp"

namespace raytiles {
    class pool {
        struct ImageJob {
            std::string path;
            std::string url;
            std::promise<Image> promise;
        };

        std::vector<std::jthread> workers;
        std::queue<ImageJob> image_queue;
        std::map<std::string, std::shared_future<Image> > in_flight_images;
        std::mutex mtx;
        std::condition_variable_any cv;

        void worker_loop(const std::stop_token &st) {
            while (true) {
                ImageJob img_job;
                {
                    std::unique_lock lock(mtx);
                    if (!cv.wait(lock, st, [this] { return !image_queue.empty(); })) return;
                    img_job = std::move(image_queue.front());
                    image_queue.pop();
                }

                try {
                    raii::image img;
                    if (auto body = fetch(img_job.path, img_job.url); body) {
                        // decode the freshly downloaded bytes directly, skipping a disk round-trip.
                        img.reset(LoadImageFromMemory(".png", reinterpret_cast<const unsigned char *>(body->data()), static_cast<int>(body->size())));
                        write_atomic(img_job.path, *body);
                    } else {
                        // tile already cached on disk.
                        img.reset(LoadImage(img_job.path.c_str()));
                    }
                    if (img->data == nullptr) throw std::runtime_error("image decode returned empty image: " + img_job.path);
                    img_job.promise.set_value(img.release());
                } catch (...) {
                    try {
                        img_job.promise.set_exception(std::current_exception());
                    } catch (...) {
                        // promise already satisfied or broken; nothing else we can do
                    }
                }
                {
                    std::lock_guard lock(mtx);
                    in_flight_images.erase(img_job.path);
                }
            }
        }

        static std::optional<std::string> fetch(const std::string &path, const std::string &url) {
            if (std::filesystem::exists(path)) return std::nullopt;

            httplib::Client cli("https://api.mapbox.com");
            cli.set_follow_location(true);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(5);
            cli.set_keep_alive(true);
#ifdef __APPLE__
            cli.enable_server_certificate_verification(false);
#endif

            auto res = cli.Get(url);
            if (!res || res->status != 200) {
                const int status = res ? res->status : -1;
                const std::string err = res ? std::string{} : httplib::to_string(res.error());
                TraceLog(LOG_WARNING, "tile download failed: %s status=%d err=%s", path.c_str(), status, err.c_str());
                throw std::runtime_error(std::format("download failed: {} status={} err={}", path, status, err));
            }
            TraceLog(LOG_DEBUG, "tile downloaded: %s", path.c_str());
            return std::move(res->body);
        }

        static void write_atomic(const std::string &path, const std::string &bytes) {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            const std::string tmp_path = path + ".tmp";
            std::FILE *f = std::fopen(tmp_path.c_str(), "wb");
            if (!f) {
                TraceLog(LOG_WARNING, "tile fopen failed: %s", tmp_path.c_str());
                throw std::runtime_error("fopen failed: " + tmp_path);
            }
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            std::error_code ec;
            std::filesystem::rename(tmp_path, path, ec);
            if (ec) {
                TraceLog(LOG_WARNING, "tile rename failed: %s -> %s (%s)", tmp_path.c_str(), path.c_str(), ec.message().c_str());
                throw std::runtime_error("rename failed: " + path);
            }
        }

    public:
        explicit pool(const int thread_count = 4) {
            workers.reserve(thread_count);
            for (int i = 0; i < thread_count; ++i) workers.emplace_back([this](const std::stop_token &st) { worker_loop(st); });
        }

        ~pool() {
            for (auto &w: workers) w.request_stop();
            workers.clear();
        }

        // returns a shared_future that resolves with the decoded Image after download.
        // if the same path is already in flight, returns the existing future.
        // if the file is already cached on disk, returns a future that resolves immediately with the loaded image.
        std::shared_future<Image> enqueue_and_load(const std::string &path, const std::string &url) {
            std::lock_guard lock(mtx);
            if (const auto it = in_flight_images.find(path); it != in_flight_images.end()) return it->second;

            std::promise<Image> promise;
            std::shared_future<Image> future = promise.get_future().share();
            in_flight_images.emplace(path, future);
            image_queue.push({path, url, std::move(promise)});
            cv.notify_one();
            return future;
        }
    };
}
