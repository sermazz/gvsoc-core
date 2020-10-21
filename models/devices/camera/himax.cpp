/*
 * Copyright (C) 2019 GreenWaves Technologies
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Authors: Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */


#include <vp/vp.hpp>
#include <vp/itf/cpi.hpp>
#include <vp/itf/i2c.hpp>
#include <unistd.h>

#include <stdint.h>
#ifdef __MAGICK__
#include <Magick++.h>
#endif

#ifdef __MAGICK__
using namespace Magick;
#endif
using namespace std;


enum {
STATE_INIT,
STATE_SOF,
STATE_WAIT_SOF,
STATE_SEND_LINE,
STATE_WAIT_EOF
};

enum {
  COLOR_MODE_CUSTOM,
  COLOR_MODE_GRAY,
  COLOR_MODE_RGB565,
  COLOR_MODE_RAW,
};

#define TP 2
#define TLINE(width) (width+144)*TP


class Himax;


class Camera_stream {

public:
    Camera_stream(Himax *top, string path, int color_mode);
    bool fetch_image();
    unsigned int get_pixel();
    void set_image_size(int width, int height, int pixel_size);

  private:
    Himax *top;
    string stream_path;
    int frame_index;
#ifdef __MAGICK__
    Image image;
#endif
    int width;
    int height;
    int pixel_size;
#ifdef __MAGICK__
    PixelPacket *image_buffer;
#endif
    int current_pixel;
    int nb_pixel;
    int color_mode;
    bool is_raw;
    uint8_t *raw_image;
};


class Himax : public vp::component
{
    friend class Camera_stream;

public:
    Himax(js::config *config);

    int build();
    void start();

protected:

    static void clock_handler(void *__this, vp::clock_event *event);
    static void i2c_sync(void *__this, int scl, int sda);

    vp::cpi_master cpi_itf;
    vp::i2c_slave i2c_itf;

    vp::clock_event *clock_event;

    vp::trace trace;

    int pclk;

    int width;
    int height;

    int nb_images;

    int color_mode;
    int pclk_value;
    int state;
    int cnt;
    int targetcnt;
    int lineptr;
    int colptr;
    int bytesel;
    int framesel;

    int vsync;
    int href;
    int data;

    int pixel_size;

    Camera_stream *stream;
};




Camera_stream::Camera_stream(Himax *top, string path, int color_mode)
 : top(top), stream_path(path), frame_index(0), current_pixel(0), nb_pixel(0), color_mode(color_mode)
{
#ifdef __MAGICK__
    image_buffer = NULL;
#endif
    raw_image = NULL;
}


void Camera_stream::set_image_size(int width, int height, int pixel_size)
{
    this->width = width;
    this->height = height;
    this->pixel_size = pixel_size;
    this->nb_pixel = width * height * pixel_size;
}


bool Camera_stream::fetch_image()
{
    char path[strlen(stream_path.c_str()) + 100];
    while(1)
    {
        sprintf(path, stream_path.c_str(), frame_index);
        this->is_raw = strstr(path, ".raw") != NULL;

        if (this->is_raw)
        {
            FILE *file = fopen(path, "r");
            if (file)
            {
                int size = this->width * this->height * this->pixel_size;
                int read_size;

                this->raw_image = new uint8_t[size];
                if ((read_size = ::fread(this->raw_image, 1, size, file)) == size)
                {
                    fclose(file);
                    break;
                }

                delete this->raw_image;
                this->raw_image = NULL;
                fclose(file);
                this->top->trace.fatal("Image file is too short(%s)\n", path);
                return false;
            }

            if (frame_index == 0)
            {
              this->top->trace.fatal("Unable to open image file (%s)\n", path);
              return false;
            }
        }
        else
        {
#ifdef __MAGICK__
          try {
              image.read(path);
              break;
          }
          catch( Exception &error_ ) {
              if (frame_index == 0) {
                  throw;
              }
          }
#else
          this->top->trace.fatal("Trying to open image file while ImageMagick has not been installed, use a raw image instead (with.raw extension) (%s)\n", path);
#endif
        }

        frame_index = 0;
    }

    //dpi_print(top->handle, ("Opened image (path: " + string(path) + ")").c_str());
    frame_index++;

    if (!this->is_raw)
    {
#ifdef __MAGICK__
        image.extent(Geometry(width, height));

        if (color_mode == COLOR_MODE_GRAY)
        {
            image.quantizeColorSpace( GRAYColorspace );
            image.quantizeColors( 256 );
            image.quantize( );
        }


        image_buffer = (PixelPacket*) image.getPixels(0, 0, width, height);
#endif
    }

    return true;
}

unsigned int Camera_stream::get_pixel()
{
#ifdef __MAGICK__
    if (image_buffer == NULL && raw_image == NULL) fetch_image();
#else
    if (raw_image == NULL) fetch_image();
#endif

    if (this->is_raw)
    {
        unsigned int result;

        if (color_mode == COLOR_MODE_GRAY || color_mode == COLOR_MODE_RAW)
        {
            result = this->raw_image[current_pixel];
        }
        else
        {
            result = (*(uint32_t *)&(this->raw_image[current_pixel*3])) & 0xFFFFFF;
        }

        current_pixel++;
        if (current_pixel == nb_pixel)
        {
            current_pixel = 0;
            delete this->raw_image;
            this->raw_image = NULL;
        }
        return result;
    }
    else
    {
#ifdef __MAGICK__
        PixelPacket *pixel = &image_buffer[current_pixel];
        current_pixel++;
        if (current_pixel == nb_pixel)
        {
            current_pixel = 0;
            image_buffer = NULL;
        }

        unsigned int shift = (sizeof(pixel->red) - 1)*8;
        if (color_mode == COLOR_MODE_GRAY)
        {
            return pixel->red >> shift;
        }
        else
        {
            unsigned char red = pixel->red >> shift;
            unsigned char green = pixel->green >> shift;
            unsigned char blue = pixel->blue >> shift;
            return (red << 16) | (green << 8) | blue;
        }
#endif
    }
    
    return 0;
}


void Himax::i2c_sync(void *__this, int scl, int sda)
{
}


void Himax::clock_handler(void *__this, vp::clock_event *event)
{
    Himax *_this = (Himax *)__this;

    _this->event_enqueue(_this->clock_event, 1);

    _this->pclk_value ^= 1;

    if (!_this->pclk_value)
    {
        switch (_this->state)
        {
            case STATE_INIT:
                _this->trace.msg(vp::trace::LEVEL_DEBUG, "State INIT\n");
                _this->cnt = 0;
                _this->targetcnt = 3*TLINE(_this->width);
                _this->state = STATE_SOF;
                _this->bytesel = 0;
                _this->framesel = 0;
                break;

            case STATE_SOF:
                if (_this->cnt == 0)
                    _this->trace.msg(vp::trace::LEVEL_DEBUG, "Starting frame\n");
                _this->trace.msg(vp::trace::LEVEL_DEBUG, "State SOF (cnt: %d, targetcnt: %d)\n", _this->cnt, _this->targetcnt);
                _this->vsync = 1;
                _this->cnt++;
                if (_this->cnt == _this->targetcnt)
                {
                    _this->cnt = 0;
                    _this->targetcnt = 17*TLINE(_this->width);
                    _this->state = STATE_WAIT_SOF;
                    _this->vsync = 0;
                }
                break;

            case STATE_WAIT_SOF:
                _this->trace.msg(vp::trace::LEVEL_DEBUG, "State WAIT_SOF (cnt: %d, targetcnt: %d)\n", _this->cnt, _this->targetcnt);
                _this->cnt++;
                if (_this->cnt == _this->targetcnt)
                {
                    _this->state = STATE_SEND_LINE;
                    _this->lineptr = 0;
                    _this->colptr = 0;
                }
                break;

            case STATE_SEND_LINE: {
                int last_byte = 1;

                _this->href = 1;

                if (_this->color_mode == COLOR_MODE_CUSTOM)
                {
                    last_byte = _this->pixel_size;
                    if (_this->stream)
                    {
                        _this->data = _this->stream->get_pixel();
                    }
                }
                else if (_this->color_mode == COLOR_MODE_GRAY)
                {
                    _this->bytesel = 1;

                    if (_this->stream)
                    {
                        _this->data = _this->stream->get_pixel();
                    }

                    //if (stimImg != NULL) {
                    //  pixel = ((uint32_t *)stimImg[framesel])[(lineptr*width)+2*colptr+offset];
                    //}

                    //data = 0.2989 * ((pixel >> 16) & 0xff) +
                    //       0.5870 * ((pixel >>  8) & 0xff) +
                    //       0.1140 * ((pixel >>  0) & 0xff);
                }
                else if (_this->color_mode == COLOR_MODE_RAW)
                {
                  _this->bytesel = 1;

                  int pixel  = 0;

                  if (_this->stream)
                  {
                    pixel = _this->stream->get_pixel();
                  }

                  // Raw bayer mode. Line 0: BGBG, Line 1: GRGR
                  int line = _this->width - _this->lineptr -1;
                  if (line & 1)
                  {
                      if (_this->colptr & 1)
                          _this->data = (pixel >> 16) & 0xff;
                      else
                          _this->data = (pixel >> 8) & 0xff;
                  }
                  else
                  {
                    if (_this->colptr & 1)
                        _this->data = (pixel >> 8) & 0xff;
                    else
                        _this->data = (pixel >> 0) & 0xff;
                  }
                }
                else
                {
                    int pixel  = 0;
                    if (_this->stream)
                    {
                      pixel = _this->stream->get_pixel();
                    }

                    //if (stimImg != NULL) {
                    //  ((uint32_t *)stimImg[framesel])[(lineptr*width)+colptr];
                    //}

                    // Coded with RGB565
                    if (_this->bytesel) _this->data = (((pixel >> 10) & 0x7) << 5) | (((pixel >> 3) & 0x1f) << 0);
                    else         _this->data = (((pixel >> 19) & 0x1f) << 3) | (((pixel >> 13) & 0x7) << 0);
                }

                if (_this->bytesel == last_byte) {
                    _this->bytesel = 0;
                    if(_this->colptr == (_this->width-1)) {
                        _this->colptr = 0;
                        if(_this->lineptr == (_this->height-1)) {
                            _this->state = STATE_WAIT_EOF;
                            _this->cnt = 0;
                            _this->targetcnt = 10*TLINE(_this->width);
                            _this->lineptr = 0;
                        } else {
                            _this->lineptr = _this->lineptr + 1;
                        }
                    } else {
                        _this->colptr = _this->colptr + 1;
                    }

                } else {
                    _this->bytesel++;
                }
                _this->trace.msg(vp::trace::LEVEL_DEBUG, "State SEND_LINE (data: 0x%x)\n", _this->data);
                break;
            }

            case STATE_WAIT_EOF:
                _this->trace.msg(vp::trace::LEVEL_DEBUG, "State WAIT_EOF (cnt: %d, targetcnt: %d)\n", _this->cnt, _this->targetcnt);
                _this->href = 0;
                _this->data = 0;
                _this->cnt++;
                if (_this->cnt == _this->targetcnt) {
                  _this->state = STATE_SOF;
                  _this->cnt = 0;
                  _this->targetcnt = 3*TLINE(_this->width);
                  _this->framesel++;
                  if (_this->framesel == _this->nb_images) _this->framesel = 0;
                }
                break;
        }
    }

    _this->cpi_itf.sync(_this->pclk_value, _this->href, _this->vsync, _this->data);
}




int Himax::build()
{
    traces.new_trace("trace", &trace, vp::DEBUG);

    this->new_master_port("cpi", &this->cpi_itf);

    this->i2c_itf.set_sync_meth(&Himax::i2c_sync);
    this->new_slave_port("i2c", &this->i2c_itf);

    this->clock_event = this->event_new(this, Himax::clock_handler);

#ifdef __MAGICK__
    InitializeMagick(NULL);
#endif

    this->stream = NULL;

    // Default color mode is 8bit gray
    std::string color_mode = get_js_config()->get("color-mode")->get_str();
    if (color_mode == "raw")
      this->color_mode = COLOR_MODE_RAW;
    else
      this->color_mode = COLOR_MODE_GRAY;

    this->width = get_js_config()->get("width")->get_int();
    this->height = get_js_config()->get("height")->get_int();
    this->pixel_size = get_js_config()->get("pixel-size")->get_int();

    // Default color mode is 16bits RGB565
    //color_mode = COLOR_MODE_RGB565;
    js::config *stream_config = get_js_config()->get("image-stream");

    if (stream_config)
    {
        string stream_path = stream_config->get_str();

        if (this->pixel_size != 0)
        {
            this->color_mode = COLOR_MODE_CUSTOM;
        }

        this->stream = new Camera_stream(this, stream_path.c_str(), this->color_mode);

        if (this->pixel_size == 0)
        {
            switch (this->color_mode)
            {
                case COLOR_MODE_GRAY:
                case COLOR_MODE_RAW:
                    this->pixel_size = 1;
                    break;

                default:
                    this->pixel_size = 2;
                    break;
            }
        }

        this->stream->set_image_size(this->width, this->height, this->pixel_size);
    }

    return 0;
}

void Himax::start()
{
    this->event_enqueue(this->clock_event, 1);

    this->pclk_value = 0;
    this->state = STATE_INIT;

    this->vsync = 0;
    this->href = 0;
    this->data = 0;
}


Himax::Himax(js::config *config)
    : vp::component(config)
{
}


extern "C" vp::component *vp_constructor(js::config *config)
{
    return new Himax(config);
}