#include "test_media_backend.h"
#include "camera_app/APC_Media.h"
#include "camera_app/binlog.h"
#include <assert.h>
#include <atomic>
#include <chrono>
#include <stdio.h>
#include <thread>

static std::atomic<bool> entered{false}, release_capture{false}, recording{false};
static std::atomic<bool> finish_on_cancel{false};
static std::atomic<unsigned> metadata_count{0};

class SurveyBackend final : public TestMediaBackend {
public:
    bool survey_available(unsigned) const override { return true; }
    bool recording() const override { return ::recording; }
    int capture_survey(const ca_survey_request &request, ca_survey_result &result,
                       const std::atomic<uint64_t> &generation) override
    {
        entered = true;
        while (!release_capture && generation == request.generation)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (generation != request.generation && !finish_on_cancel) {
            errno = ECANCELED;
            return -1;
        }
        result.frame_ms = 1234;
        return 0;
    }
};
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &)
{ return std::unique_ptr<APC_Media_Backend>(new SurveyBackend); }
void ca_log(const char *, ...) {}
bool ca_binlog_active() { return false; }
uint64_t ca_binlog_time_us() { return 0; }
void ca_binlog_emit(uint8_t, const void *, size_t) {}
int ca_backend_set_gimbal_rates(ca_backend *, float, float) { return 0; }
bool ca_survey_write_metadata(const ca_survey_result &) { ++metadata_count; return true; }

template<typename Predicate> void wait_for(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main()
{
    ca_media_config config{};
    APC_Media media(config);
    assert(media.initialize() == 0);
    ca_survey_request request{};
    request.generation = 10;
    request.index = 42;
    auto start = [&] {
        entered = false;
        release_capture = false;
        assert(media.survey_start(request));
        wait_for([] { return entered.load(); });
    };
    auto reconfigure = [&] {
        ca_config next = *media.settings();
        next.main_resolution = next.main_resolution == CA_VIDEO_720P ? CA_VIDEO_1080P : CA_VIDEO_720P;
        return media.configure(&next);
    };
    auto check_result = [&](int error) {
        ca_survey_result result{};
        wait_for([&] { return media.survey_poll(result); });
        assert(result.request.index == 42 && result.error == error);
        if (!error) assert(result.frame_ms == 1234);
        assert(!media.survey_poll(result));
    };

    // A rejected change must leave the in-flight request running.
    start();
    recording = true;
    assert(reconfigure() == -1 && errno == EBUSY);
    recording = false;
    release_capture = true;
    check_result(0);

    // Destruction of a busy worker must deliver its cancellation exactly once.
    start();
    assert(reconfigure() == 0);
    assert(!media.survey_start(request)); // Consume the previous completion first.
    check_result(ECANCELED);

    // A frame exposed before cancellation still needs its successful notice.
    finish_on_cancel = true;
    start();
    assert(reconfigure() == 0);
    check_result(0);
    finish_on_cancel = false;

    // Preserve an already completed, unpolled image through replacement too.
    start();
    release_capture = true;
    wait_for([] { return metadata_count.load() == 3; });
    assert(reconfigure() == 0);
    check_result(0);

    // Exercise queued cancellation as well as the worker-start race.
    for (unsigned i = 0; i < 30; ++i) {
        release_capture = false;
        assert(media.survey_start(request));
        assert(reconfigure() == 0);
        check_result(ECANCELED);
    }
    start();
    release_capture = true;
    check_result(0);
    puts("PASS survey completion survives pipeline changes and recording rejection");
}
