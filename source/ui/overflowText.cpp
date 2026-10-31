#include <cctype>
#include <SDL2/SDL.h>
#include <switch.h>
#include "ui/overflowText.hpp"
#include "util/theme.hpp"

namespace {
    constexpr int kMarqueeStartDelayMs = 2000;
    constexpr int kMarqueeEndPauseMs = 260;
    constexpr int kMarqueeFadeDurationMs = 260;
    constexpr int kMarqueeSpeedPxPerSec = 72;
    constexpr int kMarqueePhasePause = 0;
    constexpr int kMarqueePhaseScroll = 1;
    constexpr int kMarqueePhaseEndPause = 2;
    constexpr int kMarqueePhaseFadeOut = 3;
    constexpr int kMarqueePhaseFadeIn = 4;

    // Render hook that enables/disables SDL clip rect around marquee text.
    class OverflowClipElement : public pu::ui::elm::Element
    {
        public:
            OverflowClipElement(bool beginClip, bool* enabled, int* x, int* y, int* w, int* h)
                : beginClip(beginClip), enabled(enabled), x(x), y(y), w(w), h(h)
            {}

            static pu::ui::elm::Element::Ref New(bool beginClip, bool* enabled, int* x, int* y, int* w, int* h)
            {
                return std::make_shared<OverflowClipElement>(beginClip, enabled, x, y, w, h);
            }

            s32 GetX() override { return 0; }
            s32 GetY() override { return 0; }
            s32 GetWidth() override { return 0; }
            s32 GetHeight() override { return 0; }

            void OnRender(pu::ui::render::Renderer::Ref &Drawer, s32 X, s32 Y) override
            {
                (void)Drawer;
                (void)X;
                (void)Y;
                if (this->beginClip) {
                    if (!this->enabled || !(*this->enabled) || !this->w || !this->h || (*this->w <= 0) || (*this->h <= 0))
                        return;
                    SDL_Rect rect = { *this->x, *this->y, *this->w, *this->h };
                    SDL_RenderSetClipRect(pu::ui::render::GetMainRenderer(), &rect);
                    return;
                }
                SDL_RenderSetClipRect(pu::ui::render::GetMainRenderer(), NULL);
            }

            void OnInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos) override
            {
                (void)Down;
                (void)Up;
                (void)Held;
                (void)Pos;
            }

        private:
            bool beginClip = true;
            bool* enabled = nullptr;
            int* x = nullptr;
            int* y = nullptr;
            int* w = nullptr;
            int* h = nullptr;
    };
}

namespace inst::ui {

OverflowText::OverflowText(int fontSize, pu::ui::Color textColor)
    : textColor(textColor), backgroundColor(pu::ui::Color::FromHex("#000000FF")), fontSize(fontSize)
{
    this->baseText = pu::ui::elm::TextBlock::New(0, 0, "", fontSize);
    this->baseText->SetColor(this->textColor);

    this->probeText = pu::ui::elm::TextBlock::New(0, 0, "", fontSize);
    this->probeText->SetVisible(false);
    this->probeText->SetText("Ag");
    this->singleLineHeight = this->probeText->GetTextHeight();
    if (this->singleLineHeight <= 0)
        this->singleLineHeight = 1;
    this->probeText->SetText("");

    this->marqueeText = pu::ui::elm::TextBlock::New(0, 0, "", fontSize);
    this->marqueeText->SetColor(this->textColor);
    this->marqueeText->SetVisible(false);

    this->fadeHint0 = pu::ui::elm::Rectangle::New(0, 0, 0, 0, this->backgroundColor);
    this->fadeHint1 = pu::ui::elm::Rectangle::New(0, 0, 0, 0, this->backgroundColor);
    this->fadeHint2 = pu::ui::elm::Rectangle::New(0, 0, 0, 0, this->backgroundColor);
    this->fadeHint0->SetVisible(false);
    this->fadeHint1->SetVisible(false);
    this->fadeHint2->SetVisible(false);

    this->clipBegin = OverflowClipElement::New(true, &this->clipEnabled, &this->clipX, &this->clipY, &this->clipW, &this->clipH);
    this->clipEnd = OverflowClipElement::New(false, &this->clipEnabled, &this->clipX, &this->clipY, &this->clipW, &this->clipH);

    this->marqueeFadeRect = pu::ui::elm::Rectangle::New(0, 0, 0, 0, pu::ui::Color::FromHex("#00000000"));
    this->marqueeFadeRect->SetVisible(false);
}

void OverflowText::Attach(pu::ui::Layout* layout)
{
    if (this->attached || layout == nullptr)
        return;

    layout->Add(this->baseText);
    layout->Add(this->fadeHint0);
    layout->Add(this->fadeHint1);
    layout->Add(this->fadeHint2);
    layout->Add(this->clipBegin);
    layout->Add(this->marqueeText);
    layout->Add(this->clipEnd);
    layout->Add(this->marqueeFadeRect);
    this->attached = true;
    this->Update(true);
}

void OverflowText::SetBounds(int x, int y, int width, int height)
{
    this->x = x;
    this->y = y;
    this->width = width;
    this->height = height;
    this->updateStaticLabel();
}

void OverflowText::SetBackgroundColor(pu::ui::Color color)
{
    this->backgroundColor = color;
    this->updateFadeHintRects();
}

void OverflowText::SetText(const std::string& text)
{
    this->fullText = OverflowText::NormalizeSingleLineText(text);
    this->clippedText = this->buildClippedText(this->fullText, this->overflowing);
    this->updateStaticLabel();
    this->hideMarquee(true);
}

void OverflowText::SetSelected(bool selected, bool forceReset)
{
    const bool changed = (this->selected != selected);
    this->selected = selected;
    if (changed || forceReset) {
        const auto color = selected ? inst::theme::Color("selected_text", this->textColor) : this->textColor;
        this->baseText->SetColor(color);
        this->marqueeText->SetColor(color);
    }
    if (changed || forceReset)
        this->Update(true);
}

void OverflowText::SetVisible(bool visible)
{
    this->visible = visible;
    this->Update(true);
}

bool OverflowText::IsOverflowing() const
{
    return this->overflowing;
}

void OverflowText::Update(bool forceReset)
{
    if (!this->visible) {
        this->baseText->SetVisible(false);
        this->fadeHint0->SetVisible(false);
        this->fadeHint1->SetVisible(false);
        this->fadeHint2->SetVisible(false);
        this->hideMarquee(true);
        return;
    }

    if (forceReset)
        this->hideMarquee(true);

    this->updateStaticLabel();
    if (!this->selected || !this->overflowing || this->width <= 0 || this->height <= 0) {
        this->hideMarquee(false);
        return;
    }

    const u64 now = armGetSystemTick();
    const u64 freq = armGetSystemTickFreq();
    if (freq == 0) {
        this->hideMarquee(false);
        return;
    }

    const u64 startDelayTicks = (freq * kMarqueeStartDelayMs) / 1000;
    const u64 endPauseTicks = (freq * kMarqueeEndPauseMs) / 1000;
    const u64 fadeDurationTicks = (freq * kMarqueeFadeDurationMs) / 1000;

    if (this->marqueeText->GetText() != this->fullText)
        this->marqueeText->SetText(this->fullText);
    this->marqueeMaxOffset = this->marqueeText->GetTextWidth() - this->width;
    if (this->marqueeMaxOffset < 0)
        this->marqueeMaxOffset = 0;
    if (this->marqueeMaxOffset <= 0) {
        this->hideMarquee(false);
        return;
    }

    if (forceReset || this->marqueeLastTick == 0) {
        this->marqueeOffset = 0;
        this->marqueeLastTick = now;
        this->marqueePauseUntilTick = now + startDelayTicks;
        this->marqueeEndPauseUntilTick = 0;
        this->marqueeSpeedRemainder = 0;
        this->marqueeFadeStartTick = 0;
        this->marqueePhase = kMarqueePhasePause;
        this->marqueeFadeAlpha = 0;
    }

    // Phase flow: pause -> scroll -> end-pause -> fade-out/reset -> fade-in.
    switch (this->marqueePhase) {
        case kMarqueePhasePause:
            this->marqueeFadeAlpha = 0;
            if (now >= this->marqueePauseUntilTick) {
                this->marqueePhase = kMarqueePhaseScroll;
                this->marqueeLastTick = now;
                this->marqueeSpeedRemainder = 0;
            }
            break;
        case kMarqueePhaseScroll:
            if (now > this->marqueeLastTick) {
                const u64 elapsedTicks = now - this->marqueeLastTick;
                unsigned long long scaled = (static_cast<unsigned long long>(elapsedTicks) * static_cast<unsigned long long>(kMarqueeSpeedPxPerSec)) + static_cast<unsigned long long>(this->marqueeSpeedRemainder);
                const int advance = static_cast<int>(scaled / static_cast<unsigned long long>(freq));
                this->marqueeSpeedRemainder = static_cast<u64>(scaled % static_cast<unsigned long long>(freq));
                this->marqueeLastTick = now;
                if (advance > 0) {
                    this->marqueeOffset += advance;
                    if (this->marqueeOffset >= this->marqueeMaxOffset) {
                        this->marqueeOffset = this->marqueeMaxOffset;
                        this->marqueePhase = kMarqueePhaseEndPause;
                        this->marqueeEndPauseUntilTick = now + endPauseTicks;
                    }
                }
            }
            break;
        case kMarqueePhaseEndPause:
            this->marqueeFadeAlpha = 0;
            if (now >= this->marqueeEndPauseUntilTick) {
                this->marqueePhase = kMarqueePhaseFadeOut;
                this->marqueeFadeStartTick = now;
                this->marqueeFadeAlpha = 0;
            }
            break;
        case kMarqueePhaseFadeOut: {
            if (fadeDurationTicks == 0) {
                this->marqueeFadeAlpha = 255;
                this->marqueeOffset = 0;
                this->marqueePhase = kMarqueePhaseFadeIn;
                this->marqueeFadeStartTick = now;
            } else {
                const u64 fadeElapsed = (now > this->marqueeFadeStartTick) ? (now - this->marqueeFadeStartTick) : 0;
                if (fadeElapsed >= fadeDurationTicks) {
                    this->marqueeFadeAlpha = 255;
                    this->marqueeOffset = 0;
                    this->marqueePhase = kMarqueePhaseFadeIn;
                    this->marqueeFadeStartTick = now;
                } else {
                    this->marqueeFadeAlpha = static_cast<int>((fadeElapsed * 255ULL) / fadeDurationTicks);
                }
            }
            break;
        }
        case kMarqueePhaseFadeIn: {
            if (fadeDurationTicks == 0) {
                this->marqueeFadeAlpha = 0;
                this->marqueePhase = kMarqueePhasePause;
                this->marqueePauseUntilTick = now + startDelayTicks;
                this->marqueeLastTick = now;
            } else {
                const u64 fadeElapsed = (now > this->marqueeFadeStartTick) ? (now - this->marqueeFadeStartTick) : 0;
                if (fadeElapsed >= fadeDurationTicks) {
                    this->marqueeFadeAlpha = 0;
                    this->marqueePhase = kMarqueePhasePause;
                    this->marqueePauseUntilTick = now + startDelayTicks;
                    this->marqueeLastTick = now;
                } else {
                    this->marqueeFadeAlpha = 255 - static_cast<int>((fadeElapsed * 255ULL) / fadeDurationTicks);
                }
            }
            break;
        }
        default:
            this->marqueePhase = kMarqueePhasePause;
            this->marqueeFadeAlpha = 0;
            this->marqueePauseUntilTick = now + startDelayTicks;
            this->marqueeLastTick = now;
            this->marqueeSpeedRemainder = 0;
            break;
    }

    if (this->marqueeOffset < 0)
        this->marqueeOffset = 0;
    if (this->marqueeOffset > this->marqueeMaxOffset)
        this->marqueeOffset = this->marqueeMaxOffset;

    this->baseText->SetVisible(false);
    this->fadeHint0->SetVisible(false);
    this->fadeHint1->SetVisible(false);
    this->fadeHint2->SetVisible(false);

    this->clipEnabled = true;
    this->clipX = this->x;
    this->clipY = this->y;
    this->clipW = this->width;
    this->clipH = this->height;
    this->marqueeText->SetX(this->x - this->marqueeOffset);
    this->marqueeText->SetY(this->y + ((this->height - this->marqueeText->GetTextHeight()) / 2));
    this->marqueeText->SetVisible(true);

    if (this->marqueeFadeAlpha > 0) {
        pu::ui::Color fadeColor = this->backgroundColor;
        fadeColor.A = static_cast<u8>(this->marqueeFadeAlpha);
        this->marqueeFadeRect->SetColor(fadeColor);
        this->marqueeFadeRect->SetX(this->x);
        this->marqueeFadeRect->SetY(this->y);
        this->marqueeFadeRect->SetWidth(this->width);
        this->marqueeFadeRect->SetHeight(this->height);
        this->marqueeFadeRect->SetVisible(true);
    } else {
        this->marqueeFadeRect->SetVisible(false);
    }
}

std::vector<std::size_t> OverflowText::BuildUtf8Boundaries(const std::string& text)
{
    std::vector<std::size_t> boundaries;
    boundaries.reserve(text.size() + 1);
    boundaries.push_back(0);
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((c0 & 0x80) == 0x00) {
            len = 1;
        } else if ((c0 & 0xE0) == 0xC0) {
            if ((i + 1) < text.size()) {
                const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
                if ((c1 & 0xC0) == 0x80)
                    len = 2;
            }
        } else if ((c0 & 0xF0) == 0xE0) {
            if ((i + 2) < text.size()) {
                const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
                const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
                if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80)
                    len = 3;
            }
        } else if ((c0 & 0xF8) == 0xF0) {
            if ((i + 3) < text.size()) {
                const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
                const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
                const unsigned char c3 = static_cast<unsigned char>(text[i + 3]);
                if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80)
                    len = 4;
            }
        }
        i += len;
        boundaries.push_back(i);
    }
    return boundaries;
}

std::string OverflowText::NormalizeSingleLineText(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    bool previousWasSpace = false;
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool isWhitespace = (std::isspace(uc) != 0) || (uc < 0x20) || (uc == 0x7F);
        if (isWhitespace) {
            if (!out.empty() && !previousWasSpace)
                out.push_back(' ');
            previousWasSpace = true;
            continue;
        }
        out.push_back(c);
        previousWasSpace = false;
    }

    std::size_t start = 0;
    while (start < out.size() && out[start] == ' ')
        start++;
    std::size_t end = out.size();
    while (end > start && out[end - 1] == ' ')
        end--;
    return out.substr(start, end - start);
}

std::string OverflowText::ClipSingleLineText(const std::string& text, pu::ui::elm::TextBlock::Ref probeText, int width, int height, bool* overflow)
{
    if (overflow != nullptr)
        *overflow = false;
    if (probeText == nullptr || width <= 0 || height <= 0)
        return text;

    auto fitsSingleLine = [&](const std::string& candidate) {
        probeText->SetText(candidate);
        return (probeText->GetTextWidth() <= width)
            && (probeText->GetTextHeight() <= height);
    };

    if (fitsSingleLine(text))
        return text;

    if (overflow != nullptr)
        *overflow = true;
    const auto boundaries = OverflowText::BuildUtf8Boundaries(text);
    int low = 0;
    int high = static_cast<int>(boundaries.size()) - 1;
    int best = -1;
    while (low <= high) {
        const int mid = low + ((high - low) / 2);
        const std::string candidate = text.substr(0, boundaries[static_cast<std::size_t>(mid)]);
        if (fitsSingleLine(candidate)) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (best <= 0)
        return std::string();

    return text.substr(0, boundaries[static_cast<std::size_t>(best)]);
}

std::string OverflowText::ClipSingleLinePrefixWithSuffix(const std::string& prefix, const std::string& suffix, pu::ui::elm::TextBlock::Ref probeText, int width, int height, bool* overflow)
{
    if (overflow != nullptr)
        *overflow = false;
    if (probeText == nullptr || width <= 0 || height <= 0)
        return prefix + suffix;

    const std::string full = prefix + suffix;
    auto fitsSingleLine = [&](const std::string& candidate) {
        probeText->SetText(candidate);
        return (probeText->GetTextWidth() <= width)
            && (probeText->GetTextHeight() <= height);
    };
    if (fitsSingleLine(full))
        return full;

    if (overflow != nullptr)
        *overflow = true;
    const std::string marker = prefix.empty() ? std::string() : std::string("...");
    const auto boundaries = OverflowText::BuildUtf8Boundaries(prefix);
    int low = 0;
    int high = static_cast<int>(boundaries.size()) - 1;
    int best = -1;
    while (low <= high) {
        const int mid = low + ((high - low) / 2);
        std::string candidate = prefix.substr(0, boundaries[static_cast<std::size_t>(mid)]);
        if (!candidate.empty() && !marker.empty())
            candidate += marker;
        candidate += suffix;
        if (fitsSingleLine(candidate)) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    if (best < 0) {
        bool suffixOverflow = false;
        return OverflowText::ClipSingleLineText(suffix, probeText, width, height, &suffixOverflow);
    }

    std::string clipped = prefix.substr(0, boundaries[static_cast<std::size_t>(best)]);
    if (!clipped.empty() && !marker.empty())
        clipped += marker;
    clipped += suffix;
    return clipped;
}

std::string OverflowText::buildClippedText(const std::string& text, bool& overflow) const
{
    return OverflowText::ClipSingleLineText(text, this->probeText, this->width, this->height, &overflow);
}

void OverflowText::updateStaticLabel()
{
    if (this->baseText == nullptr)
        return;

    if (this->baseText->GetText() != this->clippedText)
        this->baseText->SetText(this->clippedText);
    // Non-overflow text is centered; overflow text is left-aligned for fade hint.
    int textX = this->x;
    if (!this->overflowing)
        textX = this->x + ((this->width - this->baseText->GetTextWidth()) / 2);
    this->baseText->SetX(textX);
    this->baseText->SetY(this->y + ((this->height - this->baseText->GetTextHeight()) / 2));
    this->baseText->SetVisible(this->visible && !(this->selected && this->overflowing));
    this->updateFadeHintRects();
}

void OverflowText::updateFadeHintRects()
{
    if (this->fadeHint0 == nullptr || this->fadeHint1 == nullptr || this->fadeHint2 == nullptr)
        return;

    const int bandWidth = (this->width >= 42) ? 14 : ((this->width > 0) ? (this->width / 3) : 0);
    const int fadeStartX = this->x + this->width - (bandWidth * 3);
    const int fadeHeight = this->height;
    pu::ui::Color c0 = this->backgroundColor;
    pu::ui::Color c1 = this->backgroundColor;
    pu::ui::Color c2 = this->backgroundColor;
    c0.A = 70;
    c1.A = 130;
    c2.A = 200;

    this->fadeHint0->SetColor(c0);
    this->fadeHint1->SetColor(c1);
    this->fadeHint2->SetColor(c2);
    this->fadeHint0->SetX(fadeStartX);
    this->fadeHint1->SetX(fadeStartX + bandWidth);
    this->fadeHint2->SetX(fadeStartX + (bandWidth * 2));
    this->fadeHint0->SetY(this->y);
    this->fadeHint1->SetY(this->y);
    this->fadeHint2->SetY(this->y);
    this->fadeHint0->SetWidth(bandWidth);
    this->fadeHint1->SetWidth(bandWidth);
    this->fadeHint2->SetWidth(bandWidth);
    this->fadeHint0->SetHeight(fadeHeight);
    this->fadeHint1->SetHeight(fadeHeight);
    this->fadeHint2->SetHeight(fadeHeight);

    const bool showHints = this->visible && this->overflowing && !this->selected && (bandWidth > 0);
    this->fadeHint0->SetVisible(showHints);
    this->fadeHint1->SetVisible(showHints);
    this->fadeHint2->SetVisible(showHints);
}

void OverflowText::hideMarquee(bool resetState)
{
    this->clipEnabled = false;
    this->marqueeText->SetVisible(false);
    this->marqueeFadeRect->SetVisible(false);
    if (!resetState)
        return;

    this->marqueeOffset = 0;
    this->marqueeMaxOffset = 0;
    this->marqueeLastTick = 0;
    this->marqueePauseUntilTick = 0;
    this->marqueeEndPauseUntilTick = 0;
    this->marqueeSpeedRemainder = 0;
    this->marqueeFadeStartTick = 0;
    this->marqueePhase = kMarqueePhasePause;
    this->marqueeFadeAlpha = 0;
}

}
