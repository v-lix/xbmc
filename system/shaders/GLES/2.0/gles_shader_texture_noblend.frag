/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#version 100

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D m_samp0;
varying vec4 m_cord0;
uniform float m_sdrPeak;
uniform float m_pqRefNits;
uniform float m_pqAccuracy;
uniform float m_pqTonemap;

#if defined(KODI_PQ_TO_SDR)
vec3 pqToSdr(vec3 pq)
{
  const float ST2084_m1 = 2610.0 / (4096.0 * 4.0);
  const float ST2084_m2 = (2523.0 / 4096.0) * 128.0;
  const float ST2084_c1 = 3424.0 / 4096.0;
  const float ST2084_c2 = (2413.0 / 4096.0) * 32.0;
  const float ST2084_c3 = (2392.0 / 4096.0) * 32.0;
  pq = clamp(pq, vec3(0.0), vec3(1.0));
  vec3 p = pow(pq, vec3(1.0 / ST2084_m2));
  vec3 linear = pow(max(p - ST2084_c1, vec3(0.0)) / (ST2084_c2 - ST2084_c3 * p),
                    vec3(1.0 / ST2084_m1));
  // linear is 0-1 representing 0-10000 nits.
  // Scale so that m_pqRefNits (default 203, BT.2408 HDR ref white) -> SDR 1.0.
  linear = linear * (10000.0 / m_pqRefNits);
  // BT.2020 -> BT.709 gamut conversion (linear domain)
  // m_pqAccuracy blends between full conversion (1.0) and identity (0.0)
  const mat3 bt2020_to_bt709 = mat3(
     1.660496, -0.124546, -0.018154,
    -0.587656,  1.132895, -0.100597,
    -0.072840, -0.008348,  1.118751);
  vec3 converted = max(bt2020_to_bt709 * linear, vec3(0.0));
  linear = mix(max(linear, vec3(0.0)), converted, m_pqAccuracy);
  // Tonemap: hard clip or Reinhard soft rolloff (preserves highlight detail)
  linear = mix(min(linear, vec3(1.0)), linear / (vec3(1.0) + linear), m_pqTonemap);
  // sRGB OETF (gamma encode with linear segment for dark values)
  vec3 lo = linear * 12.92;
  vec3 hi = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), linear));
}
#endif

void main ()
{
  vec4 rgb;

  rgb = texture2D(m_samp0, m_cord0.xy);

#if defined(KODI_PQ_TO_SDR)
  // Un-premultiply alpha before non-linear PQ conversion, then re-premultiply
  if (rgb.a > 0.0)
  {
    rgb.rgb /= rgb.a;
    rgb.rgb = pqToSdr(rgb.rgb);
    rgb.rgb *= rgb.a;
  }
#endif

#if defined(KODI_LIMITED_RANGE)
  rgb.rgb *= (235.0 - 16.0) / 255.0;
  rgb.rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ)
  rgb.rgb *= m_sdrPeak;
#endif

  gl_FragColor = rgb;
}
