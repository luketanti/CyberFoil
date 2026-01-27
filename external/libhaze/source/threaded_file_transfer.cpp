#include "haze/threaded_file_transfer.hpp"
#include "haze/thread.hpp"

#include <vector>
#include <algorithm>
#include <cstring>
#include <atomic>

namespace sphaira::thread {
namespace {

constexpr u64 BUFFER_SIZE = 1024*1024*1;

struct ThreadBuffer {
    ThreadBuffer() {
        buf.reserve(BUFFER_SIZE);
    }

    std::vector<u8> buf;
    s64 off;
};

template<std::size_t Size>
struct RingBuf {
private:
    ThreadBuffer buf[Size]{};
    unsigned r_index{};
    unsigned w_index{};

    static_assert((sizeof(RingBuf::buf) & (sizeof(RingBuf::buf) - 1)) == 0, "Must be power of 2!");

public:
    void ringbuf_reset() {
        this->r_index = this->w_index;
    }

    unsigned ringbuf_capacity() const {
        return sizeof(this->buf) / sizeof(this->buf[0]);
    }

    unsigned ringbuf_size() const {
        return (this->w_index - this->r_index) % (ringbuf_capacity() * 2U);
    }

    unsigned ringbuf_free() const {
        return ringbuf_capacity() - ringbuf_size();
    }

    void ringbuf_push(std::vector<u8>& buf_in, s64 off_in) {
        auto& value = this->buf[this->w_index % ringbuf_capacity()];
        value.off = off_in;
        std::swap(value.buf, buf_in);

        this->w_index = (this->w_index + 1U) % (ringbuf_capacity() * 2U);
    }

    void ringbuf_pop(std::vector<u8>& buf_out, s64& off_out) {
        auto& value = this->buf[this->r_index % ringbuf_capacity()];
        off_out = value.off;
        std::swap(value.buf, buf_out);

        this->r_index = (this->r_index + 1U) % (ringbuf_capacity() * 2U);
    }
};

struct ThreadData {
    ThreadData(UEvent& _uevent, s64 size, const ReadCallback& _rfunc, const WriteCallback& _wfunc, u64 buffer_size);

    auto GetResults() volatile -> Result;
    void WakeAllThreads();

    void SetReadResult(Result result) {
        read_result = result;
        if (R_FAILED(result)) {
            ueventSignal(&uevent);
        }
    }

    void SetWriteResult(Result result) {
        write_result = result;
        ueventSignal(&uevent);
    }

    auto GetWriteOffset() volatile const -> s64 {
        return write_offset;
    }

    auto GetWriteSize() const {
        return write_size;
    }

    Result readFuncInternal();
    Result writeFuncInternal();

private:
    Result SetWriteBuf(std::vector<u8>& buf, s64 size);
    Result GetWriteBuf(std::vector<u8>& buf_out, s64& off_out);

    Result Read(void* buf, s64 size, u64* bytes_read);

private:
    // these need to be copied
    UEvent& uevent;
    const ReadCallback& rfunc;
    const WriteCallback& wfunc;

    // these need to be created
    Mutex mutex{};

    CondVar can_read{};
    CondVar can_write{};

    RingBuf<2> write_buffers{};

    const u64 read_buffer_size;
    const s64 write_size;

    // these are shared between threads
    std::atomic<s64> read_offset{};
    std::atomic<s64> write_offset{};

    std::atomic<Result> read_result{Result::SuccessValue};
    std::atomic<Result> write_result{Result::SuccessValue};

    std::atomic_bool read_running{true};
    std::atomic_bool write_running{true};
};

ThreadData::ThreadData(UEvent& _uevent, s64 size, const ReadCallback& _rfunc, const WriteCallback& _wfunc, u64 buffer_size)
: uevent{_uevent}
, rfunc{_rfunc}
, wfunc{_wfunc}
, read_buffer_size{buffer_size}
, write_size{size} {
    mutexInit(std::addressof(mutex));

    condvarInit(std::addressof(can_read));
    condvarInit(std::addressof(can_write));
}

auto ThreadData::GetResults() volatile -> Result {
    R_TRY(read_result.load());
    R_TRY(write_result.load());
    R_SUCCEED();
}

void ThreadData::WakeAllThreads() {
    condvarWakeAll(std::addressof(can_read));
    condvarWakeAll(std::addressof(can_write));

    mutexUnlock(std::addressof(mutex));
}

Result ThreadData::SetWriteBuf(std::vector<u8>& buf, s64 size) {
    buf.resize(size);

    mutexLock(std::addressof(mutex));
    if (!write_buffers.ringbuf_free()) {
        if (!write_running) {
            R_SUCCEED();
        }

        R_TRY(condvarWait(std::addressof(can_read), std::addressof(mutex)));
    }

    ON_SCOPE_EXIT { mutexUnlock(std::addressof(mutex)); };
    R_TRY(GetResults());
    write_buffers.ringbuf_push(buf, 0);
    return condvarWakeOne(std::addressof(can_write));
}

Result ThreadData::GetWriteBuf(std::vector<u8>& buf_out, s64& off_out) {
    mutexLock(std::addressof(mutex));
    if (!write_buffers.ringbuf_size()) {
        if (!read_running) {
            buf_out.resize(0);
            R_SUCCEED();
        }
        R_TRY(condvarWait(std::addressof(can_write), std::addressof(mutex)));
    }

    ON_SCOPE_EXIT { mutexUnlock(std::addressof(mutex)); };
    R_TRY(GetResults());
    write_buffers.ringbuf_pop(buf_out, off_out);
    return condvarWakeOne(std::addressof(can_read));
}

Result ThreadData::Read(void* buf, s64 size, u64* bytes_read) {
    size = std::min<s64>(size, write_size - read_offset);
    const auto rc = rfunc(buf, read_offset, size, bytes_read);
    read_offset += *bytes_read;
    return rc;
}

// read thread reads all data from rfunc.
Result ThreadData::readFuncInternal() {
    ON_SCOPE_EXIT{ read_running = false; };

    // the main buffer which data is read into.
    std::vector<u8> buf;
    buf.reserve(this->read_buffer_size);

    while (this->read_offset < this->write_size && R_SUCCEEDED(this->GetResults())) {
        // read more data
        s64 read_size = this->read_buffer_size;

        u64 bytes_read{};
        buf.resize(read_size);
        R_TRY(this->Read(buf.data(), read_size, std::addressof(bytes_read)));
        if (!bytes_read) {
            break;
        }

        const auto buf_size = bytes_read;
        R_TRY(this->SetWriteBuf(buf, buf_size));
    }

    R_SUCCEED();
}

// write thread writes data to wfunc.
Result ThreadData::writeFuncInternal() {
    ON_SCOPE_EXIT{ write_running = false; };

    std::vector<u8> buf;
    buf.reserve(this->read_buffer_size);

    while (this->write_offset < this->write_size && R_SUCCEEDED(this->GetResults())) {
        s64 dummy_off;
        R_TRY(this->GetWriteBuf(buf, dummy_off));
        const auto size = buf.size();
        if (!size) {
            break;
        }

        R_TRY(this->wfunc(buf.data(), this->write_offset, buf.size()));
        this->write_offset += size;
    }

    R_SUCCEED();
}

void readFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->SetReadResult(t->readFuncInternal());
}

void writeFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->SetWriteResult(t->writeFuncInternal());
}

Result TransferInternal(s64 size, const ReadCallback& rfunc, const WriteCallback& wfunc, Mode mode, u64 buffer_size = BUFFER_SIZE) {
    if (mode == Mode::SingleThreadedIfSmaller) {
        if ((u64)size <= buffer_size) {
            mode = Mode::SingleThreaded;
        } else {
            mode = Mode::MultiThreaded;
        }
    }

    buffer_size = std::min<u64>(size, buffer_size);

    if (mode == Mode::SingleThreaded) {
        std::vector<u8> buf(buffer_size);

        s64 offset{};
        while (offset < size) {
            u64 bytes_read;
            const auto rsize = std::min<s64>(buf.size(), size - offset);
            R_TRY(rfunc(buf.data(), offset, rsize, &bytes_read));
            if (!bytes_read) {
                break;
            }

            R_TRY(wfunc(buf.data(), offset, bytes_read));

            offset += bytes_read;
        }

        R_SUCCEED();
    }
    else {
        UEvent uevent;
        ueventCreate(&uevent, false);
        ThreadData t_data{uevent, size, rfunc, wfunc, buffer_size};

        Thread t_read{};
        R_TRY(utils::CreateThread(&t_read, readFunc, std::addressof(t_data)));
        ON_SCOPE_EXIT { threadClose(&t_read); };

        Thread t_write{};
        R_TRY(utils::CreateThread(&t_write, writeFunc, std::addressof(t_data)));
        ON_SCOPE_EXIT { threadClose(&t_write); };

        R_TRY(threadStart(std::addressof(t_read)));
        R_TRY(threadStart(std::addressof(t_write)));

        ON_SCOPE_EXIT { threadWaitForExit(std::addressof(t_read)); };
        ON_SCOPE_EXIT { threadWaitForExit(std::addressof(t_write)); };

        // waits until either an error or write thread has finished.
        waitSingle(waiterForUEvent(&uevent), UINT64_MAX);

        // wait for all threads to close.
        for (;;) {
            t_data.WakeAllThreads();

            if (R_FAILED(waitSingleHandle(t_read.handle, 1000))) {
                continue;
            } else if (R_FAILED(waitSingleHandle(t_write.handle, 1000))) {
                continue;
            }
            break;
        }

        R_RETURN(t_data.GetResults());
    }
}

} // namespace

Result Transfer(s64 size, const ReadCallback& rfunc, const WriteCallback& wfunc, Mode mode) {
    return TransferInternal(size, rfunc, wfunc, mode);
}

} // namespace::thread
