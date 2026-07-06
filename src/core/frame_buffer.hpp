#pragma once
// Потокобезопасный буфер кадров, latest-wins: хранит только последний кадр.
// Развязывает поток захвата и потребителя (кодер) — нет backpressure и роста задержки.
#include "frame.hpp"
#include <memory>
#include <mutex>

namespace camctl {

class FrameBuffer {
public:
    void put(Frame f) {
        auto sp = std::make_shared<Frame>(std::move(f));
        std::lock_guard<std::mutex> lk(m_);
        latest_ = std::move(sp);
        ++seq_;
    }

    // Возвращает последний кадр (или nullptr) и его порядковый номер.
    std::shared_ptr<const Frame> get(uint64_t* seq = nullptr) const {
        std::lock_guard<std::mutex> lk(m_);
        if (seq) *seq = seq_;
        return latest_;
    }

    uint64_t seq() const {
        std::lock_guard<std::mutex> lk(m_);
        return seq_;
    }

private:
    mutable std::mutex m_;
    std::shared_ptr<Frame> latest_;
    uint64_t seq_ = 0;
};

}  // namespace camctl
