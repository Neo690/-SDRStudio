// SDRStudio - backend RTL-SDR Android.
//
// Kotlin ouvre la cle avec UsbManager et transmet le descripteur du fichier.
// libusb le reprend avec libusb_wrap_sys_device() (adaptateur Linux usbfs) et
// librtlsdr fait le reste. Aucun acces root, aucune enumeration /dev/bus/usb.
#include "rtl_android.h"

#include <rtl-sdr.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ring.h"
#include "source.h"

namespace sdr {
namespace {

std::atomic<int> g_pendingFd{-1};

const char* tunerName(enum rtlsdr_tuner type) {
    switch (type) {
        case RTLSDR_TUNER_E4000: return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T: return "R820T";
        default: return "tuner inconnu";
    }
}

class RtlSdrSource : public IqSource {
public:
    RtlSdrSource(const SourceRequest& req, int fd) : fd_(fd) {
        info_.name = "Cle RTL-SDR (USB)";
        info_.driver = "librtlsdr/0.5.3";
        info_.hardware = true;
        info_.sampleRate = req.sampleRate > 0 ? req.sampleRate : 2.4e6;
        info_.centerHz = 101.5e6;
        info_.minHz = 24e6;
        info_.maxHz = 1766e6;
    }

    ~RtlSdrSource() override { close(); }

    std::string open(std::string& err) override {
        if (dev_) return info_.name;
        if (fd_ < 0) {
            err = "descripteur USB invalide";
            return {};
        }

        const int r = rtlsdr_open_sys_device(&dev_, fd_);
        if (r < 0 || !dev_) {
            dev_ = nullptr;
            err = "rtlsdr_open_sys_device a echoue (code " + std::to_string(r) + ")";
            return {};
        }

        // Reglages par defaut : 2,4 MS/s, gain automatique du tuner.
        rtlsdr_set_sample_rate(dev_, static_cast<uint32_t>(info_.sampleRate));
        rtlsdr_set_center_freq(dev_, static_cast<uint32_t>(info_.centerHz));
        rtlsdr_set_tuner_gain_mode(dev_, 0);
        rtlsdr_set_freq_correction(dev_, 0);
        rtlsdr_reset_buffer(dev_);

        running_.store(true);
        reader_ = std::thread([this] {
            rtlsdr_read_async(dev_, &RtlSdrSource::onBulk, this, 0, 0);
        });
        // Laisse libusb demarrer les transferts avant le premier cancel possible.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        info_.name = std::string("RTL-SDR ") + tunerName(rtlsdr_get_tuner_type(dev_));
        return info_.name;
    }

    void close() override {
        if (!dev_) return;
        running_.store(false);
        rtlsdr_cancel_async(dev_);
        if (reader_.joinable()) reader_.join();
        rtlsdr_close(dev_); // ferme aussi le descripteur (libusb)
        dev_ = nullptr;
    }

    size_t read(cfloat* dst, size_t maxSamples, std::string& err) override {
        (void)err;
        return ring_.read(dst, maxSamples);
    }

    bool setCenterHz(double hz, std::string& err) override {
        if (!dev_) return false;
        if (rtlsdr_set_center_freq(dev_, static_cast<uint32_t>(hz)) != 0) {
            err = "rtlsdr_set_center_freq a echoue";
            return false;
        }
        info_.centerHz = hz;
        return true;
    }

    bool setSampleRate(double rate, std::string& err) override {
        if (!dev_) return false;
        if (std::fabs(rate - info_.sampleRate) < 1.0) return true;
        if (rtlsdr_set_sample_rate(dev_, static_cast<uint32_t>(rate)) != 0) {
            err = "rtlsdr_set_sample_rate a echoue";
            return false;
        }
        info_.sampleRate = rate;
        ring_.clear();
        rtlsdr_reset_buffer(dev_);
        return true;
    }

    bool setGainDb(double db, std::string& err) override {
        if (!dev_) return false;
        if (db < 0) { // < 0 : gain automatique
            if (rtlsdr_set_tuner_gain_mode(dev_, 0) != 0) {
                err = "rtlsdr_set_tuner_gain_mode a echoue";
                return false;
            }
            info_.gainDb = -1;
            return true;
        }
        // Gain manuel le plus proche parmi ceux acceptes par le tuner.
        int count = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (count <= 0) {
            err = "liste des gains indisponible";
            return false;
        }
        std::vector<int> gains(static_cast<size_t>(count));
        count = rtlsdr_get_tuner_gains(dev_, gains.data());
        int best = gains[0];
        for (int i = 0; i < count; ++i) {
            if (std::fabs(gains[i] - db * 10.0) < std::fabs(best - db * 10.0)) best = gains[i];
        }
        if (rtlsdr_set_tuner_gain_mode(dev_, 1) != 0 || rtlsdr_set_tuner_gain(dev_, best) != 0) {
            err = "rtlsdr_set_tuner_gain a echoue";
            return false;
        }
        info_.gainDb = best / 10.0;
        return true;
    }

    bool setPpm(int ppm, std::string& err) override {
        (void)err;
        if (!dev_) return false;
        return rtlsdr_set_freq_correction(dev_, ppm) == 0;
    }

    const SourceInfo& info() const override { return info_; }

    std::vector<double> gainsDb() const override {
        std::vector<double> out;
        if (!dev_) return out;
        int count = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (count <= 0) return out;
        std::vector<int> gains(static_cast<size_t>(count));
        count = rtlsdr_get_tuner_gains(dev_, gains.data());
        out.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) out.push_back(gains[i] / 10.0);
        return out;
    }

private:
    // Appele par librtlsdr sur le thread libusb : producteur unique du ring.
    static void onBulk(unsigned char* buf, uint32_t len, void* ctx) {
        static_cast<RtlSdrSource*>(ctx)->pushSamples(buf, len);
    }

    void pushSamples(const unsigned char* buf, uint32_t len) {
        const size_t n = len / 2;
        if (n == 0) return;
        if (scratch_.size() < n) scratch_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const float re = (static_cast<float>(buf[2 * i]) - 127.4f) / 127.4f;
            const float im = (static_cast<float>(buf[2 * i + 1]) - 127.4f) / 127.4f;
            scratch_[i] = cfloat(re, im);
        }
        ring_.write(scratch_.data(), n); // anneau plein : on abandonne le surplus
    }

    int fd_ = -1;
    rtlsdr_dev_t* dev_ = nullptr;
    std::thread reader_;
    std::atomic<bool> running_{false};
    sdr::SpscRing<cfloat> ring_{1 << 16}; // ~27 ms a 2,4 MS/s
    std::vector<cfloat> scratch_;
    SourceInfo info_;
};

} // namespace

void setPendingUsbFd(int fd) { g_pendingFd.store(fd); }

int pendingUsbFd() { return g_pendingFd.load(); }

std::unique_ptr<IqSource> createRtlSource(const SourceRequest& req, std::string& err) {
    const int fd = g_pendingFd.exchange(-1);
    if (fd < 0) {
        err = "aucune cle RTL-SDR ouverte (UsbManager n'a pas fourni de descripteur)";
        return nullptr;
    }
    return std::make_unique<RtlSdrSource>(req, fd);
}

bool rtlBackendProbe(std::string& detail) {
    const int fd = pendingUsbFd();
    detail = fd >= 0 ? "cle RTL-SDR autorisee (descripteur en attente)"
                     : "aucune cle RTL-SDR autorisee";
    return fd >= 0;
}

} // namespace sdr
