#define _CRT_SECURE_NO_WARNINGS // for uWS.h
#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <sstream>
#include <atomic>
#include <cmath>
#include <optional>
#include <filesystem>

#include <uWS/uWS.h>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "Titta/Titta.h"
#include "Titta/utils.h"
#include "function_traits.h"


void DoExitWithMsg(std::string errMsg_);

//#define LOCAL_TEST

namespace {
    // List actions
    enum class Action
    {
        Connect,

        SetSampleStreamFreq,
        StartSampleStream,
        StopSampleStream,

        SetBaseSampleFreq,
        StartSampleBuffer,
        ClearSampleBuffer,
        PeekSamples,
        StopSampleBuffer,
        SaveData,

        StoreMessage
    };

    // Map string (first input argument to mexFunction) to an Action
    const std::map<std::string, Action> actionTypeMap =
    {
        { "connect"             , Action::Connect},

        { "setSampleStreamFreq" , Action::SetSampleStreamFreq},
        { "startSampleStream"   , Action::StartSampleStream},
        { "stopSampleStream"    , Action::StopSampleStream},

        { "SetBaseSampleFreq"   , Action::SetBaseSampleFreq},
        { "startSampleBuffer"   , Action::StartSampleBuffer},
        { "clearSampleBuffer"   , Action::ClearSampleBuffer},
        { "peekSamples"         , Action::PeekSamples},
        { "stopSampleBuffer"    , Action::StopSampleBuffer},
        { "saveData"            , Action::SaveData},

        { "storeMessage"        , Action::StoreMessage},
    };

    template <bool isServer>
    void sendJson(uWS::WebSocket<isServer> *ws_, json jsonMsg_)
    {
        auto msg = jsonMsg_.dump();
        ws_->send(msg.c_str(), msg.length(), uWS::OpCode::TEXT);
    }

    template <bool isServer>
    void sendTobiiErrorAsJson(uWS::WebSocket<isServer> *ws_, TobiiResearchStatus result_, std::string errMsg_)
    {
        sendJson(ws_, {{"error", errMsg_},{"TobiiErrorCode",result_},{"TobiiErrorString",TobiiResearchStatusToString(result_)},{"TobiiErrorExplanation",TobiiResearchStatusToExplanation(result_)}});
    }

    template <bool isServer>
    void sendTobiiErrorAsJson(uWS::WebSocket<isServer> *ws_, TobiiResearchLicenseValidationResult result_, std::string errMsg_)
    {
        sendJson(ws_, { {"error", errMsg_},{"TobiiErrorCode",result_},{"TobiiErrorString",TobiiResearchLicenseValidationResultToString(result_)},{"TobiiErrorExplanation",TobiiResearchLicenseValidationResultToExplanation(result_)} });
    }

    json formatSampleAsJSON(TobiiResearchGazeData sample_)
    {
        auto lx = sample_.left_eye .gaze_point.position_on_display_area.x;
        auto ly = sample_.left_eye .gaze_point.position_on_display_area.y;
        auto rx = sample_.right_eye.gaze_point.position_on_display_area.x;
        auto ry = sample_.right_eye.gaze_point.position_on_display_area.y;
        decltype(lx) x = 0;
        decltype(ly) y = 0;

        if (sample_.left_eye.gaze_point.validity==TOBII_RESEARCH_VALIDITY_INVALID)
        {
            // just return the other eye. if also missing, so be it
            x = rx;
            y = ry;
        }
        else if (sample_.right_eye.gaze_point.validity==TOBII_RESEARCH_VALIDITY_INVALID)
        {
            // just return the other eye. if also missing, so be it
            x = lx;
            y = ly;
        }
        else
        {
            // both eyes available, average
            // could also be no eyes available, in which case this is moot and remains nan, which is fine
            x = (lx+rx)/2;
            y = (ly+ry)/2;
        }

        return
        {
            {"ts", sample_.system_time_stamp},
            {"x" , x},
            {"y" , y}
        };
    }

    void invoke_function(TobiiResearchGazeData* gaze_data_, void* ptr)
    {
        (*static_cast<std::function<void(TobiiResearchGazeData*)>*>(ptr))(gaze_data_);
    }
}

int main()
{
    // global Tobii Buffer instance
    std::unique_ptr<Titta> TittaInstance;
    TobiiResearchEyeTracker* eyeTracker = nullptr;

    uWS::Hub h;
    std::atomic<int> nClients = 0;
    int downSampFac;
    std::atomic<int> sampleTick = 0;
    std::optional<float> baseSampleFreq;
    bool needSetSampleStreamFreq = true;

    /// SERVER
    auto tobiiBroadcastCallback = [&h, &sampleTick, &downSampFac](TobiiResearchGazeData* gaze_data_)
    {
        sampleTick++;
        if ((sampleTick = sampleTick%downSampFac)!=0)
            // we're downsampling by only sending every downSampFac'th sample (e.g. every second). This is one we're not sending
            return;

        auto jsonMsg = formatSampleAsJSON(*gaze_data_);
        auto msg = jsonMsg.dump();
        h.getDefaultGroup<uWS::SERVER>().broadcast(msg.c_str(), msg.length(), uWS::OpCode::TEXT);
    };

    h.onConnection([&nClients](uWS::WebSocket<uWS::SERVER> *ws, uWS::HttpRequest req)
    {
        std::cout << "Client has connected" << std::endl;
        ws->setNoDelay(true);       // Switch off Nagle (hopefully)
        nClients++;
    });

    h.onMessage([&h, &TittaInstance, &eyeTracker, &tobiiBroadcastCallback, &downSampFac, &baseSampleFreq, &needSetSampleStreamFreq](uWS::WebSocket<uWS::SERVER> *ws, char *message, size_t length, uWS::OpCode opCode)
    {
        auto jsonInput = json::parse(std::string(message, length),nullptr,false);
        if (jsonInput.is_discarded() || jsonInput.is_null())
        {
            sendJson(ws, {{"error", "invalidJson"}});
            return;
        }
#ifdef _DEBUG
        std::cout << "Received message on server: " << jsonInput.dump(4) << std::endl;
#endif

        if (jsonInput.count("action")==0)
        {
            sendJson(ws, {{"error", "jsonMissingParam"},{"param","action"}});
            return;
        }

        // get corresponding action
        auto actionStr = jsonInput.at("action").get<std::string>();
        if (actionTypeMap.count(actionStr)==0)
        {
            sendJson(ws, {{"error", "Unrecognized action"}, {"action", actionStr}});
            return;
        }
        Action action = actionTypeMap.at(actionStr);

        switch (action)
        {
            case Action::Connect:
            {
                if (!eyeTracker)
                {
                    TobiiResearchEyeTrackers* eyetrackers = nullptr;
                    TobiiResearchStatus result = tobii_research_find_all_eyetrackers(&eyetrackers);

                    // notify if no tracker found
                    if (result != TOBII_RESEARCH_STATUS_OK)
                    {
                        sendTobiiErrorAsJson(ws, result, "Problem finding eye tracker");
                        return;
                    }

                    // select eye tracker.
                    eyeTracker = eyetrackers->eyetrackers[0];
                }

                // if license file is found in the directory, try applying it
                auto cp = std::filesystem::current_path();
                if (std::filesystem::exists("./TobiiLicense"))    // file with this name expected in cwd
                {
                    std::ifstream input("./TobiiLicense", std::ios::binary);
                    std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});

#                   define NUM_OF_LICENSES 1
                    char* license_key_ring[NUM_OF_LICENSES];
                    license_key_ring[0] = &buffer[0];
                    size_t sizes[NUM_OF_LICENSES];
                    sizes[0] = buffer.size();
                    TobiiResearchLicenseValidationResult validation_results[NUM_OF_LICENSES];
                    TobiiResearchStatus result = tobii_research_apply_licenses(eyeTracker, (const void**)license_key_ring, sizes, validation_results, NUM_OF_LICENSES);
                    if (result != TOBII_RESEARCH_STATUS_OK || validation_results[0] != TOBII_RESEARCH_LICENSE_VALIDATION_RESULT_OK)
                    {
                        if (result != TOBII_RESEARCH_STATUS_OK)
                            sendTobiiErrorAsJson(ws, result, "License file \"TobiiLicense\" found in pwd, but could not be applied.");
                        else
                            sendTobiiErrorAsJson(ws, validation_results[0], "License file \"TobiiLicense\" found in pwd, but could not be applied.");
                        return;
                    }
                }

                // get info about the connected eye tracker
                char* address;
                char* serialNumber;
                char* deviceName;
                tobii_research_get_address(eyeTracker, &address);
                tobii_research_get_serial_number(eyeTracker, &serialNumber);
                tobii_research_get_model(eyeTracker, &deviceName);

                // reply informing what eye-tracker we just connected to
                sendJson(ws, {{"action", "connect"}, {"deviceModel", deviceName}, {"serialNumber", serialNumber}, {"address", address}});

                // clean up
                tobii_research_free_string(address);
                tobii_research_free_string(serialNumber);
                tobii_research_free_string(deviceName);
            }
            break;
            case Action::SetSampleStreamFreq:
            {
                if (jsonInput.count("freq") == 0)
                {
                    sendJson(ws, {{"error", "jsonMissingParam"},{"param","freq"}});
                    return;
                }
                auto freq = jsonInput.at("freq").get<float>();

                // see what frequencies the connect device supports
                std::vector<float> frequencies;
                if (baseSampleFreq.has_value())
                    frequencies.push_back(baseSampleFreq.value());
                else
                {
                    TobiiResearchGazeOutputFrequencies* tobiiFreqs = nullptr;
                    TobiiResearchStatus result = tobii_research_get_all_gaze_output_frequencies(eyeTracker, &tobiiFreqs);
                    if (result != TOBII_RESEARCH_STATUS_OK)
                    {
                        sendTobiiErrorAsJson(ws, result, "Problem getting sampling frequencies");
                        return;
                    }
                    frequencies.insert(frequencies.end(),&tobiiFreqs->frequencies[0], &tobiiFreqs->frequencies[tobiiFreqs->frequency_count]);   // yes, pointer to one past last element
                    tobii_research_free_gaze_output_frequencies(tobiiFreqs);
                }

                // see if the requested frequency is a divisor of any of the supported frequencies, choose the best one (lowest possible frequency)
                auto best = frequencies.cend();
                downSampFac = 9999;
                for (auto x = frequencies.cbegin(); x!=frequencies.cend(); ++x)
                {
                    // is this frequency is a multiple of the requested frequency and thus in our set of potential sampling frequencies?
                    if (static_cast<int>(*x+.5f)%static_cast<int>(freq+.5f) == 0)
                    {
                        // check if this is a lower frequency than previously selecting (i.e., is the downsampling factor lower?)
                        auto tempDownSampFac = static_cast<int>(*x/freq+.5f);
                        if (tempDownSampFac<downSampFac)
                        {
                            // yes, we got a new best option
                            best = x;
                            downSampFac = tempDownSampFac;
                        }
                    }
                }
                // no matching frequency found: error
                if (best==frequencies.cend())
                {
                    if (baseSampleFreq.has_value())
                    {
                        sendJson(ws, {{"error", "invalidParam"},{"param","freq"},{"reason","requested frequency is not a divisor of the set base frequency "},{"baseFreq",baseSampleFreq.value()}});
                    }
                    else
                        sendJson(ws, {{"error", "invalidParam"},{"param","freq"},{"reason","requested frequency is not a divisor of any supported sampling frequency"}});
                    return;
                }
                // select best frequency as base frequency. Downsampling factor is already set above
                freq = *best;

                // now set the tracker to the base frequency
                TobiiResearchStatus result = tobii_research_set_gaze_output_frequency(eyeTracker, freq);
                if (result != TOBII_RESEARCH_STATUS_OK)
                {
                    sendTobiiErrorAsJson(ws, result, "Problem setting sampling frequency");
                    return;
                }

                needSetSampleStreamFreq = false;
                sendJson(ws, {{"action", "setSampleFreq"}, {"freq", freq/downSampFac}, {"baseFreq", freq}, {"status", true}});
                break;
            }
            case Action::StartSampleStream:
            {
                if (needSetSampleStreamFreq)
                {
                    sendJson(ws, {{"error", "startSampleStream"},{"reason","You have to set the stream sample rate first using action setSampleStreamFreq. NB: you also have to do this after calling setBaseSampleFreq."}});
                    return;
                }
                TobiiResearchStatus result = tobii_research_subscribe_to_gaze_data(eyeTracker, &invoke_function, new std::function<void(TobiiResearchGazeData*)>(tobiiBroadcastCallback));
                if (result != TOBII_RESEARCH_STATUS_OK)
                {
                    sendTobiiErrorAsJson(ws, result, "Problem subscribing to gaze data");
                    return;
                }

                sendJson(ws, {{"action", "startSampleStream"}, {"status", true}});
                break;
            }
            case Action::StopSampleStream:
            {
                TobiiResearchStatus result = tobii_research_unsubscribe_from_gaze_data(eyeTracker, &invoke_function);
                if (result != TOBII_RESEARCH_STATUS_OK)
                {
                    sendTobiiErrorAsJson(ws, result, "Problem unsubscribing from gaze data");
                    return;
                }

                sendJson(ws, {{"action", "stopSampleStream"}, {"status", true}});
                break;
            }

            case Action::SetBaseSampleFreq:
            {
                if (jsonInput.count("freq") == 0)
                {
                    sendJson(ws, {{"error", "jsonMissingParam"},{"param","freq"}});
                    return;
                }
                auto freq = jsonInput.at("freq").get<float>();

                // now set the tracker to the base frequency
                TobiiResearchStatus result = tobii_research_set_gaze_output_frequency(eyeTracker, freq);
                if (result != TOBII_RESEARCH_STATUS_OK)
                {
                    sendTobiiErrorAsJson(ws, result, "Problem setting sampling frequency");
                    return;
                }
                baseSampleFreq = freq;

                // user needs to reset sampleStream frequency after calling this, as downsample factor may have changed or requested may even have become unavailable
                needSetSampleStreamFreq = true;
                // also ensure no stream is currently active
                tobii_research_unsubscribe_from_gaze_data(eyeTracker, &invoke_function);

                sendJson(ws, {{"action", "setSampleFreq"}, {"freq", freq}, {"status", true}});
                break;
            }
            case Action::StartSampleBuffer:
            {
                if (!TittaInstance.get())
                    if (eyeTracker)
                        TittaInstance = std::make_unique<Titta>(eyeTracker);
                    else
                    {
                        sendJson(ws, {{"error", "startSampleBuffer"},{"reason","you need to do the \"connect\" action first"}});
                        return;
                    }

                bool status = false;
                if (TittaInstance.get())
                    status = TittaInstance.get()->start("gaze");

                sendJson(ws, {{"action", "startSampleBuffer"}, {"status", status}});
                break;
            }
            case Action::ClearSampleBuffer:
                if (TittaInstance.get())
                    TittaInstance.get()->clear("gaze");
                sendJson(ws, {{"action", "clearSampleBuffer"}, {"status", true}});  // nothing to clear or cleared, both success status
                break;
            case Action::PeekSamples:
            {
                // get sample
                auto jsonOutput = json::array();   // empty array if no samples
                if (TittaInstance.get())
                {
                    using argType = function_traits<decltype(&Titta::peekN<Titta::gaze>)>::argument<1>::type::value_type;
                    std::optional<argType> nSamples;
                    if (jsonInput.count("nSamples"))
                        nSamples = jsonInput.at("nSamples").get<argType>();

                    auto samples = TittaInstance.get()->peekN<Titta::gaze>(nSamples);
                    if (!samples.empty())
                    {
                        for (auto sample: samples)
                            jsonOutput.push_back(formatSampleAsJSON(sample));
                    }
                }

                // send
                sendJson(ws, jsonOutput);
                break;
            }
            case Action::StopSampleBuffer:
            {
                bool status = false;
                if (TittaInstance.get())
                    status = TittaInstance.get()->stop("gaze");

                sendJson(ws, {{"action", "stopSampleBuffer"}, {"status", status}});
                break;
            }
            case Action::SaveData:
            {
                if (TittaInstance.get())
                {
                    auto samples = TittaInstance.get()->consumeN<Titta::gaze>();
                    // TODO: store all to file somehow
                }
                break;
            }
            case Action::StoreMessage:
            {
                // TODO: timeStamp and store message somehow
                break;
            }
            default:
                sendJson(ws, {{"error", "Unhandled action"}, {"action", actionStr}});
                break;
        }
    });

    h.onDisconnection([&h,&nClients,&eyeTracker,&TittaInstance](uWS::WebSocket<uWS::SERVER> *ws, int code, char *message, size_t length)
    {
        std::cout << "Client disconnected, code " << code << std::endl;
        if (--nClients == 0)
        {
            std::cout << "No clients left, stopping buffering and streaming, if active..." << std::endl;
            tobii_research_unsubscribe_from_gaze_data(eyeTracker, &invoke_function);
            if (TittaInstance.get())
                TittaInstance.get()->stop("gaze");
        }
    });


#ifdef LOCAL_TEST
    /// CLIENT
    h.onConnection([](uWS::WebSocket<uWS::CLIENT> *ws, uWS::HttpRequest req)
    {
        std::cout << "Client has been notified that its connected" << std::endl;
        // start eye tracker
        sendJson(ws, {{"action", "connect"}});
        sendJson(ws, {{"action", "setSampleStreamFreq"}, {"freq", 120}});
        sendJson(ws, {{"action", "startSampleStream"}});
        sendJson(ws, {{"action", "stopSampleStream"}});
        // request sample
        sendJson(ws, {{"action", "peekSamples"}});
    });
    h.onMessage([](uWS::WebSocket<uWS::CLIENT> *ws, char *message, size_t length, uWS::OpCode opCode)
    {
        std::cout << "Received message on client: " << std::string(message, length) << std::endl;
        Sleep(10);
        sendJson(ws, {{"action", "peekSamples"}});
    });

    h.onDisconnection([&h](uWS::WebSocket<uWS::CLIENT> *ws, int code, char *message, size_t length)
    {
        std::cout << "Server has disconnected me with status code " << code << " and message: " << std::string(message, length) << std::endl;
    });
#endif

    h.listen(3003);

#ifdef LOCAL_TEST
    h.connect("ws://localhost:3003", nullptr);
#endif

    h.run();
}




// function for handling errors generated by lib
void DoExitWithMsg(std::string errMsg_)
{
    std::cout << "Error: " << errMsg_ << std::endl;
}
