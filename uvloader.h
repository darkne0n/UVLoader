/// 
/// \file uvloader.h
/// \brief Userland Vita Loader startup
/// \defgroup uvloader UVLoader
/// \brief Startup and hooks
/// @{
/// 
#ifndef UVL_MAIN
#define UVL_MAIN

#define START_SECTION __attribute__ ((section (".text.start")))

int START_SECTION uvl_entry ();
int uvl_start ();

#endif
/// @}
