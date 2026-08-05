// Tutorial : https://youtu.be/JqQEG0eipic
// Use board "ESP32C6 Dev Module" (last tested on v3.2.0)

#include "PINS_ESP32-C6-LCD-1_47.h" // GFX Library for Arduino
#include "MjpegClass.h"
#include "SD.h"
#include "Arduino.h"

#define GFX_BRIGHTNESS 255
#define MAX_FILES 20
#define MJPEG_BUFFER_SIZE (64U * 1024U)

const char *MJPEG_FOLDER = "/mjpeg";

String mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int mjpegCount = 0;
static int currentMjpegIndex = 0;

MjpegClass mjpeg;
int total_frames;
unsigned long total_read_video;
unsigned long total_decode_video;
unsigned long total_show_video;
unsigned long start_ms, curr_ms;
size_t mjpeg_buf_size;
uint8_t *mjpeg_buf = nullptr;

constexpr uint32_t LONG_PRESS_MS = 750;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;

enum class ButtonAction : uint8_t
{
    None,
    Skip,
    TogglePause
};

struct MjpegScanResult
{
    uint32_t frameCount = 0;
    uint32_t largestFrame = 0;
    bool incompleteFrame = false;
};

void setDisplayBrigthness();
void loadMjpegFilesList();
void playSelectedMjpeg(int mjpegIndex);
void mjpegPlayFromSDCard(const char *mjpegFilename);
ButtonAction pollButton();
MjpegScanResult scanMjpegFile(File &file);

void setup()
{
    Serial.begin(115200);
    DEV_DEVICE_INIT();
    delay(2000);
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    if (!gfx->begin(GFX_SPEED))
    {
        Serial.println("Display initialization failed!");
        while (true) {}
    }
    gfx->setRotation(0);
    gfx->fillScreen(RGB565_BLACK);
    setDisplayBrigthness();

    if (!SD.begin(SD_CS, SPI, 80000000, "/sd"))
    {
        Serial.println("ERROR: File system mount failed!");
        while (true) {}
    }

    // JPEG files can be much larger than their decoded image. A fixed 64 KB
    // frame buffer covers the supplied SD content while remaining bounded.
    mjpeg_buf_size = MJPEG_BUFFER_SIZE;
    mjpeg_buf = static_cast<uint8_t *>(heap_caps_malloc(mjpeg_buf_size, MALLOC_CAP_8BIT));
    if (!mjpeg_buf)
    {
        Serial.println("MJPEG buffer allocation failed!");
        while (true) {}
    }

    loadMjpegFilesList();
    pinMode(BTN_A, INPUT);
}

void setDisplayBrigthness()
{
    ledcAttachChannel(GFX_BL, 1000, 8, 1);
    ledcWrite(GFX_BL, GFX_BRIGHTNESS);
}

void loop()
{
    if (mjpegCount == 0)
    {
        delay(1000);
        return;
    }

    playSelectedMjpeg(currentMjpegIndex);
    currentMjpegIndex = (currentMjpegIndex + 1) % mjpegCount;
}

void playSelectedMjpeg(int mjpegIndex)
{
    if (mjpegIndex < 0 || mjpegIndex >= mjpegCount)
    {
        return;
    }

    const String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[mjpegIndex];
    char mjpegFilename[128];
    if (fullPath.length() >= sizeof(mjpegFilename))
    {
        Serial.println("ERROR: MJPEG path is too long");
        return;
    }
    fullPath.toCharArray(mjpegFilename, sizeof(mjpegFilename));
    Serial.printf("Playing %s\n", mjpegFilename);
    mjpegPlayFromSDCard(mjpegFilename);
}

int jpegDrawCallback(JPEGDRAW *pDraw)
{
    if (!pDraw || pDraw->x < 0 || pDraw->y < 0 ||
        pDraw->x >= gfx->width() || pDraw->y >= gfx->height() ||
        pDraw->iWidth <= 0 || pDraw->iHeight <= 0 ||
        pDraw->x + pDraw->iWidth > gfx->width() ||
        pDraw->y + pDraw->iHeight > gfx->height())
    {
        return 1;
    }

    const unsigned long started = millis();
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    total_show_video += millis() - started;
    return 1;
}

ButtonAction pollButton()
{
    static bool wasPressed = false;
    static uint32_t pressedAt = 0;
    const bool pressed = digitalRead(BTN_A) == LOW;
    const uint32_t now = millis();

    if (pressed && !wasPressed)
    {
        wasPressed = true;
        pressedAt = now;
    }
    else if (!pressed && wasPressed)
    {
        wasPressed = false;
        const uint32_t heldFor = now - pressedAt;
        if (heldFor < BUTTON_DEBOUNCE_MS)
        {
            return ButtonAction::None;
        }
        return (heldFor >= LONG_PRESS_MS) ? ButtonAction::TogglePause : ButtonAction::Skip;
    }
    return ButtonAction::None;
}

void mjpegPlayFromSDCard(const char *mjpegFilename)
{
    File file = SD.open(mjpegFilename, "r");
    if (!file || file.isDirectory())
    {
        Serial.printf("ERROR: Failed to open %s file for reading\n", mjpegFilename);
        return;
    }

    Serial.println(F("MJPEG start"));
    gfx->fillScreen(RGB565_BLACK);
    start_ms = curr_ms = millis();
    total_frames = 0;
    total_read_video = 0;
    total_decode_video = 0;
    total_show_video = 0;
    bool paused = false;

    if (!mjpeg.setup(&file, mjpeg_buf, mjpeg_buf_size, jpegDrawCallback, true,
                     0, 0, gfx->width(), gfx->height()))
    {
        Serial.println("ERROR: MJPEG parser setup failed");
        file.close();
        return;
    }

    while (file.available())
    {
        const ButtonAction action = pollButton();
        if (action == ButtonAction::Skip)
        {
            Serial.println(F("Playback skipped"));
            break;
        }
        if (action == ButtonAction::TogglePause)
        {
            paused = !paused;
            Serial.println(paused ? F("Playback paused") : F("Playback resumed"));
        }
        if (paused)
        {
            delay(10);
            continue;
        }
        if (!mjpeg.readMjpegBuf())
        {
            break;
        }

        total_read_video += millis() - curr_ms;
        curr_ms = millis();
        if (mjpeg.drawJpg())
        {
            total_frames++;
        }
        else
        {
            Serial.println("WARNING: Rejected malformed JPEG frame");
        }
        total_decode_video += millis() - curr_ms;
        curr_ms = millis();
    }

    const unsigned long time_used = millis() - start_ms;
    Serial.println(F("MJPEG end"));
    file.close();
    const unsigned long decode_only = (total_decode_video >= total_show_video)
                                          ? total_decode_video - total_show_video
                                          : 0;
    const float fps = time_used ? (1000.0f * total_frames / time_used) : 0.0f;
    Serial.printf("Total frames: %d\n", total_frames);
    if (mjpeg.getOversizeFrameCount())
    {
        Serial.printf("Skipped oversized frames: %lu (buffer: %u bytes)\n",
                      static_cast<unsigned long>(mjpeg.getOversizeFrameCount()),
                      static_cast<unsigned int>(mjpeg_buf_size));
    }
    Serial.printf("Time used: %lu ms\n", time_used);
    Serial.printf("Average FPS: %0.1f\n", fps);
    if (time_used)
    {
        Serial.printf("Read MJPEG: %lu ms (%0.1f %%)\n", total_read_video, 100.0f * total_read_video / time_used);
        Serial.printf("Decode video: %lu ms (%0.1f %%)\n", decode_only, 100.0f * decode_only / time_used);
        Serial.printf("Show video: %lu ms (%0.1f %%)\n", total_show_video, 100.0f * total_show_video / time_used);
    }
}

MjpegScanResult scanMjpegFile(File &file)
{
    MjpegScanResult result;
    uint8_t buffer[READ_BUFFER_SIZE];
    bool inFrame = false;
    bool havePrevious = false;
    uint8_t previous = 0;
    uint32_t frameSize = 0;

    file.seek(0);
    while (true)
    {
        const size_t bytesRead = file.read(buffer, sizeof(buffer));
        if (bytesRead == 0)
        {
            break;
        }
        for (size_t i = 0; i < bytesRead; i++)
        {
            const uint8_t value = buffer[i];
            if (!inFrame)
            {
                if (havePrevious && previous == 0xFF && value == 0xD8)
                {
                    inFrame = true;
                    frameSize = 2;
                }
            }
            else
            {
                if (frameSize != UINT32_MAX)
                {
                    frameSize++;
                }
                if (previous == 0xFF && value == 0xD9)
                {
                    result.frameCount++;
                    if (frameSize > result.largestFrame)
                    {
                        result.largestFrame = frameSize;
                    }
                    inFrame = false;
                }
            }
            previous = value;
            havePrevious = true;
        }
    }
    result.incompleteFrame = inFrame;
    file.seek(0);
    return result;
}

void loadMjpegFilesList()
{
    File mjpegDir = SD.open(MJPEG_FOLDER);
    if (!mjpegDir)
    {
        Serial.printf("Failed to open %s folder\n", MJPEG_FOLDER);
        return;
    }

    mjpegCount = 0;
    while (mjpegCount < MAX_FILES)
    {
        File file = mjpegDir.openNextFile();
        if (!file)
        {
            break;
        }
        if (!file.isDirectory())
        {
            const String name = file.name();
            if (name.endsWith(".mjpeg"))
            {
                mjpegFileList[mjpegCount] = name;
                mjpegFileSizes[mjpegCount] = file.size();
                const MjpegScanResult scan = scanMjpegFile(file);
                const bool compatible = scan.frameCount > 0 &&
                                        !scan.incompleteFrame &&
                                        scan.largestFrame <= mjpeg_buf_size;
                Serial.printf("Check: %s - %lu frames, largest %lu bytes: %s%s\n",
                              name.c_str(),
                              static_cast<unsigned long>(scan.frameCount),
                              static_cast<unsigned long>(scan.largestFrame),
                              compatible ? "OK" : "UNSUPPORTED",
                              scan.incompleteFrame ? " (incomplete frame)" : "");
                mjpegCount++;
            }
        }
        file.close();
    }
    mjpegDir.close();

    Serial.printf("%d mjpeg files read\n", mjpegCount);
    for (int i = 0; i < mjpegCount; i++)
    {
        Serial.printf("File %d: %s, Size: %lu bytes\n", i, mjpegFileList[i].c_str(),
                      static_cast<unsigned long>(mjpegFileSizes[i]));
    }
}
