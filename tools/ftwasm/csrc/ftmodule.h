/*
 * Module list for the browser build: only what TrueType/OpenType (glyf and
 * CFF outlines) need with FT_LOAD_FORCE_AUTOHINT and the smooth renderer.
 * Same relative order as FreeType's default include/freetype/config/ftmodule.h,
 * so a font is offered to the drivers in the same sequence as the native build.
 * pshinter is left out: the auto-hinter loads outlines with FT_LOAD_NO_SCALE,
 * so the PostScript hinter is never reached.
 */
FT_USE_MODULE(FT_Module_Class, autofit_module_class)
FT_USE_MODULE(FT_Driver_ClassRec, tt_driver_class)
FT_USE_MODULE(FT_Driver_ClassRec, cff_driver_class)
FT_USE_MODULE(FT_Module_Class, psaux_module_class)
FT_USE_MODULE(FT_Module_Class, psnames_module_class)
FT_USE_MODULE(FT_Module_Class, sfnt_module_class)
FT_USE_MODULE(FT_Renderer_Class, ft_smooth_renderer_class)
