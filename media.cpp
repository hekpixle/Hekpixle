// Hekpixle — media tools (vision, image generation, TTS, STT).
// Mirrors lib/media.js. Calls OpenAI-compatible /chat/completions (vision),
// /images/generations, /audio/speech and /audio/transcriptions endpoints.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace hek::media {

namespace {
std::string base64Encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { out.push_back(tbl[(val>>valb)&0x3F]); valb -= 6; }
    }
    if (valb > -6) out.push_back(tbl[((val<<8)>>(valb+8))&0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}
}

void registerTools(Context& ctx) {
    ctx.add({"vision", "media", "read",
        "Describe an image (file path or URL). Uses the configured vision model "
        "(media.visionModel). Returns the model's description.",
        json::parse(R"JS({"type":"object","properties":{"image":{"type":"string","description":"local file path or http(s) URL"},"prompt":{"type":"string","description":"what to look for (default: describe)"}},"required":["image"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            auto s = ctx.loadSettings();
            auto m = s.value("media", json::object());
            std::string baseUrl = m.value("visionBaseUrl", s.value("baseUrl","https://api.openai.com/v1"));
            std::string apiKey  = m.value("visionApiKey", s.value("apiKey",""));
            std::string model   = m.value("visionModel", "gpt-4o-mini");
            if (apiKey.empty()) return "ERROR: vision API key not set";
            std::string img = a.value("image","");
            std::string dataUrl;
            if (img.rfind("http",0) == 0) dataUrl = img;
            else {
                std::ifstream in(img, std::ios::binary);
                if (!in) return "ERROR: cannot read image: " + img;
                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string ext = "png";
                if (img.size() > 4) { auto e = img.substr(img.size()-4); if (e==".jpg"||e=="jpeg") ext="jpeg"; }
                dataUrl = "data:image/" + ext + ";base64," + base64Encode(content);
            }
            json parts = json::array();
            parts.push_back(json{{"type","text"},{"text", a.value("prompt","Describe this image in detail.")}});
            parts.push_back(json{{"type","image_url"},{"image_url", json{{"url", dataUrl}}}});
            json userMsg = json{{"role","user"},{"content", parts}};
            json body = json{{"model", model},{"messages", json::array({userMsg})}};
            auto r = httpFetch("POST", baseUrl + "/chat/completions", body.dump(),
                {{"Content-Type","application/json"},{"Authorization","Bearer " + apiKey}}, "");
            if (!r.error.empty()) return std::string("ERROR: ") + r.error;
            try {
                json resp = json::parse(r.body);
                if (resp.contains("error")) return "ERROR: " + resp["error"].dump();
                return resp["choices"][0]["message"]["content"].get<std::string>();
            } catch (...) { return r.body; }
        }
    });

    ctx.add({"image_generation", "media", "write",
        "Generate an image from a text prompt. Saves to userDataDir/generated_<n>.png and returns the path.",
        json::parse(R"JS({"type":"object","properties":{"prompt":{"type":"string"},"size":{"type":"string","enum":["256x256","512x512","1024x1024"]}},"required":["prompt"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            auto s = ctx.loadSettings();
            auto m = s.value("media", json::object());
            std::string baseUrl = m.value("baseUrl", s.value("baseUrl","https://api.openai.com/v1"));
            std::string apiKey  = m.value("apiKey", s.value("apiKey",""));
            std::string model   = m.value("imageModel", "dall-e-3");
            if (apiKey.empty()) return "ERROR: API key not set";
            json body = {{"model",model},{"prompt",a.value("prompt","")},{"size",a.value("size","1024x1024")}};
            auto r = httpFetch("POST", baseUrl + "/images/generations", body.dump(),
                {{"Content-Type","application/json"},{"Authorization","Bearer " + apiKey}}, "");
            if (!r.error.empty()) return std::string("ERROR: ") + r.error;
            try {
                json j = json::parse(r.body);
                if (j.contains("data") && !j["data"].empty()) {
                    std::string url = j["data"][0].value("url","");
                    if (!url.empty()) return url;  // agent can web_extract to fetch it
                    std::string b64 = j["data"][0].value("b64_json","");
                    if (!b64.empty()) {
                        std::string path = ctx.userDataDir() + "\\generated.png";
                        // decode base64 (simple)
                        std::string out; std::vector<int> T(256,-1);
                        for (int i=0;i<64;i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]]=i;
                        int val=0,valb=-8;
                        for (unsigned char c : b64) { if (T[c]==-1) continue; val=(val<<6)+T[c]; valb+=6; if (valb>=0){out.push_back(char((val>>valb)&0xFF)); valb-=8;} }
                        std::ofstream f(path, std::ios::binary); f << out;
                        return path;
                    }
                }
                return "ERROR: no image in response: " + r.body;
            } catch (const std::exception& e) { return std::string("ERROR: ") + e.what(); }
        },
        [&ctx](const json& a) -> json { return {{"title","تولید تصویر"},{"detail",a.value("prompt","")}}; }
    });

    ctx.add({"tts", "media", "write",
        "Convert text to speech. Saves a WAV/MP3 to userDataDir/tts_<n>.<ext> and returns the path.",
        json::parse(R"JS({"type":"object","properties":{"text":{"type":"string"},"voice":{"type":"string"}},"required":["text"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            auto s = ctx.loadSettings();
            auto m = s.value("media", json::object());
            std::string baseUrl = m.value("baseUrl", s.value("baseUrl","https://api.openai.com/v1"));
            std::string apiKey  = m.value("apiKey", s.value("apiKey",""));
            std::string model   = m.value("ttsModel", "tts-1");
            std::string voice   = m.value("ttsVoice", a.value("voice","alloy"));
            if (apiKey.empty()) return "ERROR: API key not set";
            json body = {{"model",model},{"input",a.value("text","")},{"voice",voice}};
            auto r = httpFetch("POST", baseUrl + "/audio/speech", body.dump(),
                {{"Content-Type","application/json"},{"Authorization","Bearer " + apiKey}}, "");
            if (!r.error.empty()) return std::string("ERROR: ") + r.error;
            std::string path = ctx.userDataDir() + "\\tts.mp3";
            std::ofstream f(path, std::ios::binary); f << r.body;
            return path;
        }
    });

    ctx.add({"stt", "media", "read",
        "Transcribe an audio file (path) to text. Returns the transcript.",
        json::parse(R"JS({"type":"object","properties":{"file":{"type":"string"}},"required":["file"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            auto s = ctx.loadSettings();
            auto m = s.value("media", json::object());
            std::string baseUrl = m.value("baseUrl", s.value("baseUrl","https://api.openai.com/v1"));
            std::string apiKey  = m.value("apiKey", s.value("apiKey",""));
            std::string model   = m.value("sttModel", "whisper-1");
            if (apiKey.empty()) return "ERROR: API key not set";
            std::string file = a.value("file","");
            std::ifstream in(file, std::ios::binary);
            if (!in) return "ERROR: cannot read audio file";
            std::string audio((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string b64 = base64Encode(audio);
            // multipart-ish: simplest is to send raw audio with the model header;
            // many providers accept application/octet-stream + model header.
            auto r = httpFetch("POST", baseUrl + "/audio/transcriptions", audio,
                {{"Authorization","Bearer " + apiKey},{"Content-Type","audio/mpeg"},{"X-Model",model}}, "");
            if (!r.error.empty()) return std::string("ERROR: ") + r.error;
            return r.body;
        }
    });
}

} // namespace hek::media
