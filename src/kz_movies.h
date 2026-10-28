#pragma once

class PS2Runtime;

// Skip intro movies option (killzone.ini [Game] SkipIntro, KZ_SKIP_INTRO=0/1); KZ_MOVIE_LOG=1 logs movie names.
// Skip logos option ([Game] SkipLogos, KZ_SKIP_LOGOS=0/1): the boot logo screens run through in a few frames.
void kzMoviesInstall(PS2Runtime &runtime);
