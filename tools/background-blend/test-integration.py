#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise production render admission and effective alpha with a recording renderer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
def function(source, signature):
    begin = source.index(signature)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]

code = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <vector>
namespace UTILS { namespace COLOR { using Color=unsigned; } }
enum {RENDER_ORDER_ALL_BACK_TO_FRONT, RENDER_ORDER_BACK_TO_FRONT, RENDER_ORDER_FRONT_TO_BACK};
enum {RENDER_STEREO_MODE_OFF, RENDER_STEREO_MODE_MONO};
struct Gfx {
 int order{RENDER_ORDER_BACK_TO_FRONT},stereo{RENDER_STEREO_MODE_OFF}; unsigned alpha{255};
 int GetRenderOrder(){return order;}int GetStereoMode(){return stereo;}
 float GetTransformDepth(){return .25f;}
 unsigned MergeColor(unsigned c){return (c&0x00ffffff)|(((c>>24)*alpha/255)<<24);}
};
struct Window {Gfx gfx;Gfx& GetGfxContext(){return gfx;}};
struct Settings {int m_guiBackgroundBlendRes{1080};Settings* GetAdvancedSettings(){return this;}};
static Window window;static Settings settings;
struct CServiceBroker {
 static Window* GetWinSystem(){return &window;}
 static Settings* GetSettingsComponent(){return &settings;}
};
struct Texture {bool alpha{false};bool HasAlpha()const{return alpha;}};
struct Array {std::vector<std::shared_ptr<Texture>>m_textures;size_t size()const{return m_textures.size();}};
struct CGUITexture {
 bool background{true},ready{true};int draws{0};unsigned m_alpha{128},m_diffuseColor{0xffffffff};
 struct Info{unsigned diffuseColor{0};}m_info;
 Array m_texture{{std::make_shared<Texture>()}},m_diffuse;size_t m_currentFrame{0};
 bool HasAlpha()const; bool ReadyToRender(){return ready;} bool IsBackgroundRequest(){return background;}
 void Render(){++draws;}
};
namespace GUI_RENDER_TIMING {
 enum class Stage{Fade};struct Scope{Scope(Stage,bool){}};
 bool Active(){return false;}void FadeLayers(unsigned){}
}
namespace GUI_BACKGROUND_BLEND {
 static bool available{true};static unsigned attempts{0};
 bool Draw(unsigned resolution,float depth,const std::function<void()>&draw){
  ++attempts;assert(resolution==1080&&depth==.25f);if(!available)return false;draw();return true;}
}
struct CGUIControl {unsigned renders{0};void Render(){++renders;}};
struct CGUIImage:CGUIControl {
 struct CFadingTexture {std::unique_ptr<CGUITexture>m_texture{std::make_unique<CGUITexture>()};};
 std::unique_ptr<CGUITexture>m_texture{std::make_unique<CGUITexture>()};
 std::vector<std::unique_ptr<CFadingTexture>> owners;
 std::vector<CFadingTexture*>m_fadingTextures;
 bool IsVisible(){return true;}void Render();
 void Old(){owners.emplace_back(std::make_unique<CFadingTexture>());m_fadingTextures.push_back(owners.back().get());}
};
'''
code += function((root/'xbmc/guilib/GUITexture.cpp').read_text(), 'bool CGUITexture::HasAlpha() const')
code += function((root/'xbmc/guilib/GUIImage.cpp').read_text(), 'void CGUIImage::Render()')
code += r'''
int main(){
 using namespace GUI_BACKGROUND_BLEND;
 {CGUIImage i;i.Old();i.Render();assert(attempts==1&&i.m_texture->draws==1&&i.m_fadingTextures[0]->m_texture->draws==1&&i.renders==1);}
 // Unavailable helper takes the ordinary draw path exactly once per image.
 available=false;{CGUIImage i;i.Old();i.Render();assert(i.m_texture->draws==1&&i.m_fadingTextures[0]->m_texture->draws==1);}available=true;
 // All bypasses avoid the helper, even with a fade still present.
 for(int mode=0;mode<6;mode++){
  window=Window{};settings=Settings{};CGUIImage i;i.Old();
  if(mode==0)settings.m_guiBackgroundBlendRes=0;
  if(mode==1)i.m_texture->background=false;
  if(mode==2)window.gfx.order=RENDER_ORDER_FRONT_TO_BACK;
  if(mode==3)window.gfx.stereo=RENDER_STEREO_MODE_MONO;
  if(mode==4)i.m_texture->ready=false;
  if(mode==5)i.m_fadingTextures[0]->m_texture->m_alpha=255;
  const auto previous=attempts;i.Render();assert(attempts==previous);}
 window=Window{};settings=Settings{};
 {CGUIImage i;const auto previous=attempts;i.Render();assert(attempts==previous&&i.m_texture->draws==1);}
 // Actual effective-alpha test includes skin tint, control and group opacity,
 // source-image alpha and a diffuse alpha texture.
 CGUITexture t;t.m_alpha=255;assert(!t.HasAlpha());
 t.m_info.diffuseColor=0x37ffffff;assert(t.HasAlpha());t.m_info.diffuseColor=0;
 window.gfx.alpha=100;assert(t.HasAlpha());window.gfx.alpha=255;
 t.m_texture.m_textures[0]->alpha=true;assert(t.HasAlpha());t.m_texture.m_textures[0]->alpha=false;
 t.m_diffuse.m_textures.emplace_back(std::make_shared<Texture>());t.m_diffuse.m_textures[0]->alpha=true;assert(t.HasAlpha());
}
'''
with tempfile.TemporaryDirectory(prefix='background-admission-') as temporary:
    path=Path(temporary);(path/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                    '-DTARGET_LINUX','-DHAS_LIBAMCODEC','-DHAS_GLES=2',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS production render admission: debug off, active fade, readiness, opaque passes, stereo, setting, fallback, completed fade and effective alpha')
