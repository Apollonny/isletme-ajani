#include "ajan.hpp"
#include <iostream>
#include <pjsua2.hpp>
using namespace pj;
struct ProbeCall : Call {
    AudioMediaPlayer player;
    AudioMediaRecorder recorder;
    fs::path input, output;
    std::atomic<bool> active = false, ended = false;
    ProbeCall(Account &a, fs::path i, fs::path o) : Call(a), input(i), output(o) {}
    void onCallState(OnCallStateParam &) override {
        auto s = getInfo().state;
        if (s == PJSIP_INV_STATE_CONFIRMED)
            active = true;
        if (s == PJSIP_INV_STATE_DISCONNECTED)
            ended = true;
    }
    void onCallMediaState(OnCallMediaStateParam &) override {
        if (getInfo().state == PJSIP_INV_STATE_DISCONNECTED)
            return;
        auto media = getAudioMedia(-1);
        player.createPlayer(input.string(), PJMEDIA_FILE_NO_LOOP);
        recorder.createRecorder(output.string());
        player.startTransmit(media);
        media.startTransmit(recorder);
    }
};
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc < 3)
            throw std::runtime_error("Usage: SipProbe input-16k.wav output.wav");
        Endpoint ep;
        ep.libCreate();
        EpConfig ec;
        ec.logConfig.level = 1;
        ec.medConfig.clockRate = 16000;
        ec.medConfig.sndClockRate = 16000;
        ep.libInit(ec);
        TransportConfig tc;
        tc.port = 5061;
        tc.boundAddress = "127.0.0.1";
        ep.transportCreate(PJSIP_TRANSPORT_UDP, tc);
        ep.libStart();
        ep.audDevManager().setNullDev();
        Account account;
        AccountConfig ac;
        ac.idUri = "sip:probe@127.0.0.1";
        account.create(ac);
        {
            ProbeCall call(account, argv[1], argv[2]);
            CallOpParam p(true);
            call.makeCall("sip:agent@127.0.0.1:5060", p);
            double start = clock_ms();
            while (!call.ended && clock_ms() - start < 16000) {
                ep.libHandleEvents(50);
                Sleep(10);
            }
            if (!call.ended) {
                CallOpParam end;
                call.hangup(end);
                Sleep(250);
            }
        }
        account.shutdown();
        ep.libDestroy();
        std::cout << "Local SIP/RTP recording saved\n";
        return 0;
    } catch (const Error &e) {
        std::cerr << e.info() << "\n";
        return 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
