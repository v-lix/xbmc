#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual image transition methods with deterministic image readiness."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'xbmc/guilib/GUIImage.cpp').read_text()
header = (root / 'xbmc/guilib/GUIImage.h').read_text()

def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cctype>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
namespace UTILS { namespace COLOR { using Color = unsigned; } }
namespace StringUtils {
bool EndsWithNoCase(const std::string& value, const std::string& suffix) {
 return value.size()>=suffix.size() && std::equal(suffix.rbegin(),suffix.rend(),value.rbegin(),
 [](unsigned char a,unsigned char b){return std::tolower(a)==std::tolower(b);});
}
}
using CDirtyRegionList = std::vector<int>;
static std::set<std::string> readyNames{"a", "b", "c", "d", "last"};
struct Color { bool HasInfo() const { return false; } };
struct CGUITexture {
  struct TextureInfo { std::string filename{"a"}; bool useLarge{true}; } m_info;
  bool ready{true}, background{true}, cache{true}, m_continuation{false}; unsigned alpha{255};
  CGUITexture* Clone() const { return new CGUITexture(*this); }
  bool ReadyToRender() const { return ready; }
  bool IsBackgroundRequest() const { return background; }
  bool FailedToAlloc() const { return false; }
  const std::string& GetFileName() const { return m_info.filename; }
  bool SetFileName(const std::string& value);
  void SetUseCache(bool value) { cache=value; }
  bool AllocResources() { ready=readyNames.count(m_info.filename); return false; }
  void FreeResources(bool = false) { ready=false; }
  bool Process(unsigned) { return AllocResources(); }
  bool SetAlpha(unsigned value) { bool changed=alpha!=value;alpha=value;return changed; }
  bool SetDiffuseColor(unsigned) { return false; }
  Color GetDiffuseColor() const { return {}; }
};
struct Advanced { bool m_guiLimitBackgroundFades{true}; };
struct Settings { Advanced advanced; Advanced* GetAdvancedSettings(){return &advanced;} };
struct Large { bool motion{false}; bool IsBackgroundMotion() const {return motion;} };
struct GUI { Large large; Large& GetLargeTextureManager(){return large;} };
struct Gfx { float GetFPS() const {return 60;} };
struct Window { Gfx gfx; Gfx& GetGfxContext(){return gfx;} };
static Settings settings; static GUI gui; static Window window;
struct CServiceBroker {
 static Settings* GetSettingsComponent(){return &settings;}
 static GUI* GetGUI(){return &gui;}
 static Window* GetWinSystem(){return &window;}
};
struct Info {
 bool constant{false};
 void SetLabel(const std::string&,const std::string&,int){constant=true;}
 std::string GetFallback() const {return "fallback";}
 bool IsConstant() const {return constant;}
};
struct CGUIControl {void Process(unsigned,CDirtyRegionList&){} };
struct CGUIImage: CGUIControl {
FADING_TEXTURE
 std::unique_ptr<CGUITexture> m_texture{std::make_unique<CGUITexture>()};
 std::vector<CFadingTexture*> m_fadingTextures;
 std::string m_currentTexture{"a"},m_currentFallback;
 std::optional<std::pair<std::string,bool>> m_pendingBackground;
 unsigned m_crossFadeTime{300},m_currentFadeTime{300},m_lastRenderTime{1};
 unsigned m_diffuseColor{0xffffffff},m_lastDiffuseColor{0xffffffff};
 int m_parentID{0}; Info m_info;
 int GetParentID() const {return m_parentID;}
 ~CGUIImage(){for(auto* t:m_fadingTextures)delete t;}
 void SetFileName(const std::string&,bool=false,bool=true);
 void Process(unsigned,CDirtyRegionList&);
 bool ProcessFading(CFadingTexture*,unsigned,unsigned,bool&);
 unsigned char GetFadeLevel(unsigned) const;
 void FreeTextures(bool=false);
 void MarkDirtyRegion(){} void UpdateDiffuseColor(void*){}
};
'''.replace('FADING_TEXTURE', function(header, '  class CFadingTexture') + ';')
code += function((root / 'xbmc/guilib/GUITexture.cpp').read_text(), 'bool CGUITexture::SetFileName(')
for signature in ['void CGUIImage::SetFileName(', 'void CGUIImage::Process(',
                  'bool CGUIImage::ProcessFading(', 'unsigned char CGUIImage::GetFadeLevel(',
                  'void CGUIImage::FreeTextures(']:
    code += '\n' + function(source, signature)
code += r'''
int main(){
 CDirtyRegionList dirty;
 // Repeated selections preserve the running pair and admit only the latest.
 {CGUIImage i;i.SetFileName("b");i.Process(17,dirty);
  for(const auto& name:{"c","d","last"}){i.SetFileName(name);assert(i.m_fadingTextures.size()==1);}
  assert(i.m_currentTexture=="b" && i.m_pendingBackground->first=="last");
  for(unsigned t=33;t<=961;t+=16){i.Process(t,dirty);assert(i.m_fadingTextures.size()<=1);}
  assert(i.m_currentTexture=="last"&&!i.m_pendingBackground&&i.m_fadingTextures.empty());}
 // A return to the current incoming image withdraws the pending selection.
 {CGUIImage i;i.SetFileName("b");i.Process(17,dirty);i.SetFileName("c");i.SetFileName("b");
  assert(!i.m_pendingBackground);for(unsigned t=33;t<=401;t+=16)i.Process(t,dirty);
  assert(i.m_currentTexture=="b");}
 // A not-yet-visible incoming request may be replaced without keeping it.
 {CGUIImage i;i.SetFileName("missing");i.Process(17,dirty);i.SetFileName("last");
  assert(i.m_fadingTextures.size()==1&&!i.m_pendingBackground&&i.m_currentTexture=="last");}
 // Pending cache policy is applied to its own image, not the active pair.
 {CGUIImage i;i.SetFileName("b");i.Process(17,dirty);i.SetFileName("last",false,false);
  assert(i.m_texture->cache);for(unsigned t=33;t<=401;t+=16)i.Process(t,dirty);
  assert(!i.m_texture->cache);}
 // Existing motion suppression and retirement remain valid.
 {CGUIImage i;i.SetFileName("b");i.Process(17,dirty);i.SetFileName("last");
  gui.large.motion=true;i.Process(33,dirty);gui.large.motion=false;
  assert(i.m_fadingTextures.size()<=1);i.FreeTextures();assert(!i.m_pendingBackground&&i.m_fadingTextures.empty());}
 // Resource eviction retains the newest constant filename and cache policy.
 {CGUIImage i;i.SetFileName("b",true);i.Process(17,dirty);i.SetFileName("last",true,false);
  i.FreeTextures();assert(!i.m_pendingBackground&&i.m_texture->GetFileName()=="last"&&!i.m_texture->cache);}
 // Restoring a queued GIF must preserve SetFileName's loader/cache bypass.
 {CGUIImage i;i.SetFileName("b",true);i.Process(17,dirty);i.SetFileName("last.gif",true);
  i.FreeTextures();assert(i.m_texture->GetFileName()=="last.gif"&&!i.m_texture->cache&&!i.m_texture->m_info.useLarge);}
 // Foreground artwork and the explicit compatibility switch retain old behavior.
 for(bool foreground:{false,true}){CGUIImage i;i.m_texture->background=!foreground;
  settings.advanced.m_guiLimitBackgroundFades=foreground;i.SetFileName("b");i.Process(17,dirty);i.SetFileName("c");
  assert(i.m_fadingTextures.size()==2&&!i.m_pendingBackground);}
 // Many rapid retargets never grow the outgoing stack or lose the final request.
 settings.advanced.m_guiLimitBackgroundFades=true;
 {CGUIImage i;for(unsigned n=1;n<=2000;n++){
    i.SetFileName(n%2?"b":"c");i.Process(1+n*16,dirty);assert(i.m_fadingTextures.size()<=1);}
  i.SetFileName("last");for(unsigned t=32017;t<=33009;t+=16)i.Process(t,dirty);
  assert(i.m_currentTexture=="last"&&!i.m_pendingBackground&&i.m_fadingTextures.empty());}
}
'''
with tempfile.TemporaryDirectory(prefix='background-fades-') as temporary:
    path = Path(temporary)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS production fade methods: bounded pair, latest selection, reversals, readiness, cache policy, motion, retirement, compatibility and 2000 retargets')
