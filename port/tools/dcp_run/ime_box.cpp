// See ime_box.h.
#include "ime_box.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "hle/ime.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

namespace {

// UTF-16 units of a UTF-8 string (the game's maxTextLength counts those).
uint32_t utf16_units(const char* s, int len) {
    uint32_t n = 0;
    for (int i = 0; i < len; ++i) {
        uint8_t b = uint8_t(s[i]);
        if ((b & 0xc0) != 0x80) n += b >= 0xf0 ? 2 : 1;
    }
    return n;
}

// Cuts the field back to maxTextLength as the player types or pastes.
int clamp_length(ImGuiInputTextCallbackData* d) {
    const uint32_t max = *static_cast<const uint32_t*>(d->UserData);
    while (d->BufTextLen > 0 && utf16_units(d->Buf, d->BufTextLen) > max) {
        int i = d->BufTextLen - 1;
        while (i > 0 && (uint8_t(d->Buf[i]) & 0xc0) == 0x80) --i;  // start of the last code point
        d->DeleteChars(i, d->BufTextLen - i);
    }
    return 0;
}

}  // namespace

bool overlay_input_held() {
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; ++i)
        if (keys[i]) return true;
    return SDL_GetMouseState(nullptr, nullptr) != 0;
}

namespace {
void setup_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8, st.FrameRounding = 5, st.WindowPadding = ImVec2(22, 18), st.FramePadding = ImVec2(10, 7);
    st.ItemSpacing = ImVec2(10, 12);
    // Built large and scaled down per frame (FontGlobalScale) to follow the window size. Segoe UI covers Latin-1.
    const char* kFont = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (!SDL_GetPathInfo(kFont, nullptr) || !io.Fonts->AddFontFromFileTTF(kFont, 40.0f)) {
        ImFontConfig fc;
        fc.SizePixels = 26.0f;
        io.Fonts->AddFontDefault(&fc);
    }
}
}  // namespace

bool ImeBox::init(SDL_Window* win, void* gl_context) {
    setup_imgui();
    ok_ = ImGui_ImplSDL3_InitForOpenGL(win, gl_context) && ImGui_ImplOpenGL3_Init("#version 330");
    if (!ok_) std::fprintf(stderr, "ime: ImGui init failed; name entry keeps the current name\n");
    return ok_;
}

bool ImeBox::init_gpu(SDL_Window* win, SDL_GPUDevice* dev) {
    setup_imgui();
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = dev;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ok_ = ImGui_ImplSDL3_InitForSDLGPU(win) && ImGui_ImplSDLGPU3_Init(&info);
    gpu_ = ok_;
    if (!ok_) std::fprintf(stderr, "ime: ImGui init failed; name entry keeps the current name\n");
    return ok_;
}

void ImeBox::gpu_prepare(void* cmd) {
    if (gpu_ && drawn_) Imgui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), static_cast<SDL_GPUCommandBuffer*>(cmd));
}

void ImeBox::gpu_render(void* cmd, void* pass) {
    if (!gpu_ || !drawn_) return;
    drawn_ = false;
    ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), static_cast<SDL_GPUCommandBuffer*>(cmd),
                                     static_cast<SDL_GPURenderPass*>(pass));
}

void ImeBox::shutdown() {
    if (!ok_) return;
    if (gpu_) ImGui_ImplSDLGPU3_Shutdown();
    else ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    ok_ = false;
}

void ImeBox::handle_event(const SDL_Event& e) {
    if (ok_ && id_) ImGui_ImplSDL3_ProcessEvent(&e);  // only while open: ImGui queues events until its next frame
}

bool ImeBox::update(SDL_Window* win) {
    drawn_ = false;
    if (!ok_) {
        dcp::hle::ImeRequest r;
        if (dcp::hle::ime_pending(&r)) dcp::hle::ime_finish(r.id, nullptr);  // no UI: keep the old name
        return false;
    }
    if (!id_) {
        dcp::hle::ImeRequest r;
        if (dcp::hle::ime_pending(&r)) {
            id_ = r.id, max_ = r.max_len ? r.max_len : 16, title_ = r.title, focus_ = true, blocking_ = true;
            // ImGui only saw input while the last box was open, so the Enter that closed it is still "held" there and
            // its key repeat would close this one at once (empty text = cancel). Start from a clean slate, and take
            // no OK / cancel until the key that opened this box is released.
            ImGuiIO& io = ImGui::GetIO();
            io.ClearEventsQueue();
            io.ClearInputKeys();
            io.ClearInputMouse();
            armed_ = false;
            size_t n = std::min(r.text.size(), sizeof buf_ - 1);
            std::memcpy(buf_, r.text.data(), n);
            buf_[n] = 0;
        } else {
            if (blocking_ && !overlay_input_held()) blocking_ = false;
            return false;
        }
    } else if (dcp::hle::ImeRequest r; !dcp::hle::ime_pending(&r) || r.id != id_) {
        id_ = 0;  // the game aborted or replaced the dialog
        SDL_StopTextInput(win);
        return false;
    }
    return true;
}

void ImeBox::begin_frame(SDL_Window* win) {
    int w = 1, h = 1;
    SDL_GetWindowSize(win, &w, &h);
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = std::clamp(float(h) / 1080.0f, 0.45f, 2.0f) * 0.7f;
    if (gpu_) ImGui_ImplSDLGPU3_NewFrame();
    else ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImeBox::end_frame() {
    ImGui::Render();
    if (gpu_) drawn_ = true;
    else ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ImeBox::draw(SDL_Window* win) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));

    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs * 22, 0), ImGuiCond_Always);
    bool done = false, cancel = false;
    ImGui::Begin("##ime", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(title_.empty() ? "Enter a name" : title_.c_str());
    ImGui::SetNextItemWidth(-1);
    if (focus_) ImGui::SetKeyboardFocusHere(), focus_ = false;
    if (ImGui::InputText("##text", buf_, sizeof buf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit,
                         clamp_length, &max_))
        done = true;
    ImGui::TextDisabled("%u / %u   Enter: OK   Esc: cancel", utf16_units(buf_, int(std::strlen(buf_))), max_);
    const float bw = fs * 6;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("OK", ImVec2(bw, 0))) done = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
    ImGui::End();

    if (!armed_) {
        armed_ = !overlay_input_held();
        done = cancel = false;
    }
    if (done || cancel) {
        std::string text = buf_;
        // An empty name is not something the PS4 keyboard hands back for these fields: treat it as cancel.
        if (cancel || text.find_first_not_of(' ') == std::string::npos) dcp::hle::ime_finish(id_, nullptr);
        else dcp::hle::ime_finish(id_, &text);
        id_ = 0;
        SDL_StopTextInput(win);
    }
}
