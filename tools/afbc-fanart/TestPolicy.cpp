/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "guilib/AMLAFBCTexturePolicy.h"

#include <cassert>
#include <iostream>
using namespace AML_AFBC;
int main()
{
  // Existing fanart-only users retain the old opaque/background/aspect policy.
  assert(Classify(1920, 1080, true, false, true, false) == ImageClass::Background);
  assert(Classify(480, 720, false, false, true, false) == ImageClass::None);
  assert(Classify(1920, 1080, true, true, true, false) == ImageClass::None);
  assert(Classify(1920, 1080, false, false, true, false) == ImageClass::None);
  // Selective artwork includes portrait, square, landscape and alpha images.
  assert(Classify(480, 720, false, false, false, true) == ImageClass::Artwork);
  assert(Classify(300, 450, false, false, false, true) == ImageClass::Artwork);
  assert(Classify(512, 512, false, true, false, true) == ImageClass::Artwork);
  assert(Classify(1920, 1080, true, true, false, true) == ImageClass::Background);
  assert(Classify(256, 512, false, false, false, true) == ImageClass::Artwork);
  assert(Classify(256, 511, false, false, false, true) == ImageClass::None);
  assert(Classify(256, 256, false, false, false, true) == ImageClass::None);
  assert(Classify(2048, 64, false, false, false, true) == ImageClass::None);
  assert(Classify(2048, 128, false, false, false, true) == ImageClass::Artwork);
  assert(Classify(1, 1, false, true, false, true) == ImageClass::None);
  assert(Classify(4096, 4096, false, false, false, true) == ImageClass::Artwork);
  assert(Classify(4097, 4096, false, false, false, true) == ImageClass::None);
  assert(Classify(0, 1080, true, false, true, true) == ImageClass::None);
  assert(Classify(1920, 1080, true, false, false, false) == ImageClass::None);
  std::cout << "PASS legacy fanart and selective artwork policy, size boundaries, alpha, disabled "
               "and bounds\n";
}
