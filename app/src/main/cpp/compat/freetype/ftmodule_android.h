// FreeType 模块表（精简版）。
//
// FreeType 自带的 ftmodule.h 列了 19 个模块（含 PFR / Type42 / WinFNT / PCF / BDF /
// SDF / SVG 等），而 ftinit.c 会为其中每一项生成引用——少编一个就会链接失败，
// 全编又引入一堆用不到的代码。这里通过 FT_CONFIG_MODULES_H 指向本文件，
// 只保留 Android 移植实际需要的模块：
//
//   TrueType 轮廓（.ttf/.ttc，含 msgothic.ttc 这类字体集合）
//   CFF/OpenType 轮廓（.otf；Android 的 NotoSansCJK-Regular.ttc 正是 CFF 轮廓，
//     这也是不能用 stb_truetype 之类精简光栅器的原因）
//   autofit 自动微调、psaux/psnames/pshinter 支撑 CFF
//   smooth（灰度抗锯齿）与 raster1（单色）两个渲染器

FT_USE_MODULE(FT_Module_Class, autofit_module_class)
FT_USE_MODULE(FT_Driver_ClassRec, tt_driver_class)
FT_USE_MODULE(FT_Driver_ClassRec, cff_driver_class)
FT_USE_MODULE(FT_Module_Class, sfnt_module_class)
FT_USE_MODULE(FT_Module_Class, psaux_module_class)
FT_USE_MODULE(FT_Module_Class, psnames_module_class)
FT_USE_MODULE(FT_Module_Class, pshinter_module_class)
FT_USE_MODULE(FT_Renderer_Class, ft_smooth_renderer_class)
FT_USE_MODULE(FT_Renderer_Class, ft_raster1_renderer_class)
