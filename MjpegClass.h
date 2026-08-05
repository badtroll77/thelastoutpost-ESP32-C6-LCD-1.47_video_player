/*******************************************************************************
 * JPEGDEC Wrapper Class
 *
 * Dependent libraries:
 * JPEGDEC: https://github.com/bitbank2/JPEGDEC.git
 ******************************************************************************/
#ifndef _MJPEGCLASS_H_
#define _MJPEGCLASS_H_

#define READ_BUFFER_SIZE 1024

/* Wio Terminal */
#if defined(ARDUINO_ARCH_SAMD) && defined(SEEED_GROVE_UI_WIRELESS)
#include <Seeed_FS.h>
#elif defined(ESP32) || defined(ESP8266)
#include <FS.h>
#else
#include <SD.h>
#endif

#include <JPEGDEC.h>

class MjpegClass
{
public:
  MjpegClass() : _input(nullptr), _mjpeg_buf(nullptr), _mjpeg_buf_size(0),
                 _pfnDraw(nullptr), _read_buf(nullptr), _read_pos(0),
                 _buf_read(0), _mjpeg_buf_offset(0), _useBigEndian(false),
                 _x(0), _y(0), _widthLimit(0), _heightLimit(0), _scale(-1),
                 _oversize_frames(0) {}

  ~MjpegClass()
  {
    free(_read_buf);
  }

  bool setup(Stream *input, uint8_t *mjpeg_buf, size_t mjpeg_buf_size,
             JPEG_DRAW_CALLBACK *pfnDraw, bool useBigEndian,
             int x, int y, int widthLimit, int heightLimit)
  {
    if (!input || !mjpeg_buf || mjpeg_buf_size < 4 || !pfnDraw ||
        widthLimit <= 0 || heightLimit <= 0)
    {
      return false;
    }

    _input = input;
    _mjpeg_buf = mjpeg_buf;
    _mjpeg_buf_size = mjpeg_buf_size;
    _pfnDraw = pfnDraw;
    _useBigEndian = useBigEndian;
    _x = x;
    _y = y;
    _widthLimit = widthLimit;
    _heightLimit = heightLimit;
    _read_pos = 0;
    _buf_read = 0;
    _mjpeg_buf_offset = 0;
    _scale = -1;
    _oversize_frames = 0;

    if (!_read_buf)
    {
      _read_buf = static_cast<uint8_t *>(malloc(READ_BUFFER_SIZE));
    }
    return _read_buf != nullptr;
  }

  // Returns true only for a complete, bounded JPEG frame. Malformed and
  // oversize frames are discarded so later frames in the stream can play.
  bool readMjpegBuf()
  {
    while (true)
    {
      uint8_t previous = 0;
      uint8_t value = 0;
      bool have_previous = false;
      bool found_soi = false;

      while (readByte(value))
      {
        if (have_previous && previous == 0xFF && value == 0xD8)
        {
          found_soi = true;
          break;
        }
        previous = value;
        have_previous = true;
      }
      if (!found_soi)
      {
        return false;
      }

      _mjpeg_buf[0] = 0xFF;
      _mjpeg_buf[1] = 0xD8;
      _mjpeg_buf_offset = 2;
      if (readFrame())
      {
        return true;
      }

      // readFrame() only fails after consuming an invalid frame or EOF.
      // Continue here (without recursion) so a run of bad frames cannot grow
      // the stack and valid later frames remain playable.
      if (_read_pos >= _buf_read && !_input->available())
      {
        return false;
      }
    }
  }

  bool drawJpg()
  {
    if (!_mjpeg_buf || _mjpeg_buf_offset < 4 || !_pfnDraw)
    {
      return false;
    }

    // JPEGDEC open()/decode() return 1 for success and 0 for failure.
    // JPEG_SUCCESS (0) belongs to getLastError(), not these two methods.
    if (!_jpeg.openRAM(_mjpeg_buf, _mjpeg_buf_offset, _pfnDraw))
    {
      return false;
    }

    bool decoded = false;
    int w = _jpeg.getWidth();
    int h = _jpeg.getHeight();
    if (w > 0 && h > 0)
    {
      int scale = 0;
      int scaled_w = w;
      int scaled_h = h;
      int max_mcus = _widthLimit / 16;
      if (h > _heightLimit * 4)
      {
        scale = JPEG_SCALE_EIGHTH;
        scaled_w /= 8;
        scaled_h /= 8;
        max_mcus = _widthLimit / 2;
      }
      else if (h > _heightLimit * 2)
      {
        scale = JPEG_SCALE_QUARTER;
        scaled_w /= 4;
        scaled_h /= 4;
        max_mcus = _widthLimit / 4;
      }
      else if (h > _heightLimit)
      {
        scale = JPEG_SCALE_HALF;
        scaled_w /= 2;
        scaled_h /= 2;
        max_mcus = _widthLimit / 8;
      }

      _jpeg.setMaxOutputSize(max_mcus > 0 ? max_mcus : 1);
      _jpeg.setPixelType(_useBigEndian ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);
      const int draw_x = (scaled_w > _widthLimit) ? 0 : (_widthLimit - scaled_w) / 2;
      const int draw_y = (scaled_h > _heightLimit) ? 0 : (_heightLimit - scaled_h) / 2;
      decoded = _jpeg.decode(draw_x, draw_y, scale) != 0;
    }

    _jpeg.close();
    return decoded;
  }

  uint32_t getOversizeFrameCount() const
  {
    return _oversize_frames;
  }

private:
  bool readByte(uint8_t &value)
  {
    if (_read_pos >= _buf_read)
    {
      _buf_read = _input->readBytes(_read_buf, READ_BUFFER_SIZE);
      _read_pos = 0;
      if (_buf_read == 0)
      {
        return false;
      }
    }
    value = _read_buf[_read_pos++];
    return true;
  }

  bool readFrame()
  {
    uint8_t previous = 0xD8;
    uint8_t value = 0;
    bool overflow = false;

    while (readByte(value))
    {
      if (!overflow)
      {
        if (_mjpeg_buf_offset >= static_cast<int32_t>(_mjpeg_buf_size))
        {
          overflow = true;
        }
        else
        {
          _mjpeg_buf[_mjpeg_buf_offset++] = value;
        }
      }

      if (previous == 0xFF && value == 0xD9)
      {
        if (!overflow && _mjpeg_buf_offset >= 4)
        {
          return true;
        }
        // An oversize frame is malformed for this player. Its EOI has now
        // been consumed, so the caller can safely scan for the next SOI.
        _oversize_frames++;
        return false;
      }
      previous = value;
    }
    return false;
  }

  Stream *_input;
  uint8_t *_mjpeg_buf;
  size_t _mjpeg_buf_size;
  JPEG_DRAW_CALLBACK *_pfnDraw;
  uint8_t *_read_buf;
  size_t _read_pos;
  size_t _buf_read;
  int32_t _mjpeg_buf_offset;
  bool _useBigEndian;
  int _x;
  int _y;
  int _widthLimit;
  int _heightLimit;
  int _scale;
  uint32_t _oversize_frames;
  JPEGDEC _jpeg;
};

#endif // _MJPEGCLASS_H_
