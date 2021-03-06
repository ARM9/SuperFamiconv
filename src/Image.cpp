#include "Image.h"

namespace sfc {

Image::Image(const std::string& path) {
  std::vector<unsigned char> buffer;
  unsigned w, h;

  unsigned error = lodepng::load_file(buffer, path);
  if (error) throw std::runtime_error(lodepng_error_text(error));

  lodepng::State state;
  state.decoder.color_convert = false;
  error = lodepng::decode(_data, w, h, state, buffer);
  if (error) throw std::runtime_error(lodepng_error_text(error));

  bool needs_conversion = false;

  if (state.info_raw.colortype == LCT_PALETTE) {
    _indexed_data = _data;
    for (unsigned i = 0; i < state.info_raw.palettesize * 4; i += 4) {
      uint32_t color = (state.info_raw.palette[i]) + (state.info_raw.palette[i + 1] << 8) +
                       (state.info_raw.palette[i + 2] << 16) + (state.info_raw.palette[i + 3] << 24);
      _palette.push_back(color);
    }
    needs_conversion = true;
    state.info_raw.colortype = LCT_RGBA;
  }

  if (state.info_png.color.colortype == LCT_RGB ||
      state.info_png.color.colortype == LCT_GREY ||
      state.info_png.color.colortype == LCT_GREY_ALPHA) {
    state.info_raw.colortype = LCT_RGBA;
    needs_conversion = true;
  }

  if (state.info_png.color.bitdepth != 8) {
    state.info_raw.bitdepth = 8;
    needs_conversion = true;
  }

  if (needs_conversion) {
    _data.clear();
    state.decoder.color_convert = true;
    error = lodepng::decode(_data, w, h, state, buffer);
    if (error) throw std::runtime_error(lodepng_error_text(error));
  }

  _width = w;
  _height = h;
}

Image::Image(const sfc::Palette& palette) {
  auto v = palette.normalized_colors();
  if (v.empty() || v[0].empty()) throw std::runtime_error("No colors");

  _width = palette.max_colors_per_subpalette();
  _height = (unsigned)v.size();
  _data.resize(_width * _height * 4);
  std::fill(_data.begin(), _data.end(), 0);

  for (unsigned y = 0; y < v.size(); ++y) {
    auto vy = v[y];
    for (unsigned x = 0; x < vy.size(); ++x) set_pixel(sfc::rgba_color(vy[x]), x, y);
  }
}

Image::Image(const sfc::Tileset& tileset) {
  const auto tiles = tileset.tiles();
  const unsigned image_width = 128;
  const unsigned tile_width = tileset.tile_width();
  const unsigned tile_height = tileset.tile_height();
  const unsigned tiles_per_row = sfc::div_ceil(image_width, tile_width);
  const unsigned rows = sfc::div_ceil(tileset.size(), tiles_per_row);

  _width = image_width;
  _height = rows * tileset.tile_height();
  _data.resize(_width * _height * 4);
  std::fill(_data.begin(), _data.end(), 0);

  for (unsigned tile_index = 0; tile_index < tiles.size(); ++tile_index) {
    auto tile_rgba = tiles[tile_index].rgba_data();
    blit(tile_rgba, (tile_index % tiles_per_row) * tile_width, (tile_index / tiles_per_row) * tile_height, tile_width);
  }
}

// Make new normalized image with color indices mapped to palette
Image::Image(const Image& image, const sfc::Subpalette& subpalette) {
  _palette = subpalette.get_normalized_colors();
  if (_palette.empty()) throw std::runtime_error("No colors");

  sfc::Mode mode = subpalette.mode();
  _width = image.width();
  _height = image.height();
  unsigned size = _width * _height;
  _indexed_data.resize(size);
  _data.resize(size * 4);

  for (unsigned i = 0; i < size; ++i) {
    rgba_t color = sfc::normalize_color(sfc::reduce_color(image.rgba_color_at(i), mode), mode);
    if (color == transparent_color) {
      _indexed_data[i] = 0;
      set_pixel(transparent_color, i);
    } else {
      size_t palette_index = std::find(_palette.begin(), _palette.end(), color) - _palette.begin();
      if (palette_index < _palette.size()) {
        _indexed_data[i] = (index_t)palette_index;
        set_pixel(sfc::rgba_color(_palette[palette_index]), i);
      } else {
        throw std::runtime_error("Color not in palette");
      }
    }
  }
}

std::vector<rgba_t> Image::rgba_data() const {
  return sfc::to_rgba(_data);
}

Image Image::crop(unsigned x, unsigned y, unsigned crop_width, unsigned crop_height) const {
  Image img;
  img._palette = _palette;
  img._width = crop_width;
  img._height = crop_height;
  img._data.resize(crop_width * crop_height * 4);

  uint32_t fillval = transparent_color;
  size_t fillsize = img._data.size();
  for (size_t i = 0; i < fillsize; i += 4) std::memcpy(img._data.data() + i, &fillval, sizeof(fillval));

  if (x > _width || y > _height) {
    // Crop outside source image: return empty
    if (_indexed_data.size()) img._indexed_data.resize(crop_width * crop_height);
    return img;
  }

  unsigned blit_width = (x + crop_width > _width) ? _width - x : crop_width;
  unsigned blit_height = (y + crop_height > _height) ? _height - y : crop_height;

  for (unsigned iy = 0; iy < blit_height; ++iy) {
    std::memcpy(&img._data[iy * img._width * 4], &_data[(x * 4) + ((iy + y) * _width * 4)], blit_width * 4);
  }

  if (_indexed_data.size()) {
    img._indexed_data.resize(crop_width * crop_height);
    for (unsigned iy = 0; iy < blit_height; ++iy) {
      std::memcpy(&img._indexed_data[iy * img._width], &_indexed_data[x + ((iy + y) * _width)], blit_width);
    }
  }
  return img;
}

std::vector<Image> Image::crops(unsigned tile_width, unsigned tile_height) const {
  std::vector<Image> v;
  unsigned x = 0;
  unsigned y = 0;
  while (y < _height) {
    while (x < _width) {
      v.push_back(crop(x, y, tile_width, tile_height));
      x += tile_width;
    }
    x = 0;
    y += tile_width;
  }
  return v;
}

std::vector<std::vector<rgba_t>> Image::rgba_crops(unsigned tile_width, unsigned tile_height) const {
  std::vector<std::vector<rgba_t>> v;
  unsigned x = 0;
  unsigned y = 0;
  while (y < _height) {
    while (x < _width) {
      v.push_back(crop(x, y, tile_width, tile_height).rgba_data());
      x += tile_width;
    }
    x = 0;
    y += tile_width;
  }
  return v;
}

std::vector<std::vector<index_t>> Image::indexed_crops(unsigned tile_width, unsigned tile_height) const {
  if (!_indexed_data.size()) throw std::runtime_error("No indexed data in image");
  std::vector<std::vector<index_t>> v;
  unsigned x = 0;
  unsigned y = 0;
  while (y < _height) {
    while (x < _width) {
      v.push_back(crop(x, y, tile_width, tile_height).indexed_data());
      x += tile_width;
    }
    x = 0;
    y += tile_width;
  }
  return v;
}

void Image::save(const std::string& path) const {
  unsigned error = lodepng::encode(path.c_str(), _data, _width, _height, LCT_RGBA, 8);
  if (error) throw std::runtime_error(lodepng_error_text(error));
}

std::ostream& operator<<(std::ostream& os, const Image& img) {
  std::stringstream ss;
  ss << img.width() << "x" << img.height() << ", " << (img.palette_size() ? "indexed color" : "rgb color");
  return os << ss.str();
}

// nb! set_pixel & blit doesn't affect indexed data
inline void Image::set_pixel(const rgba_t color, const unsigned index) {
  const unsigned offset = index * 4;
  if ((offset + 3) > _data.size()) return;
  _data[offset + 0] = (channel_t)(color & 0xff);
  _data[offset + 1] = (channel_t)((color >> 8) & 0xff);
  _data[offset + 2] = (channel_t)((color >> 16) & 0xff);
  _data[offset + 3] = (channel_t)((color >> 24) & 0xff);
}

inline void Image::set_pixel(const rgba_t color, const unsigned x, const unsigned y) {
  const unsigned offset = ((y * _width) + x) * 4;
  if ((offset + 3) > _data.size()) return;
  _data[offset + 0] = (channel_t)(color & 0xff);
  _data[offset + 1] = (channel_t)((color >> 8) & 0xff);
  _data[offset + 2] = (channel_t)((color >> 16) & 0xff);
  _data[offset + 3] = (channel_t)((color >> 24) & 0xff);
}

void Image::blit(const std::vector<rgba_t>& rgba_data, const unsigned x, const unsigned y, const unsigned width) {
  for (unsigned i = 0; i < rgba_data.size(); ++i) set_pixel(rgba_data[i], (i % width) + x, (i / width) + y);
}

} /* namespace sfc */
