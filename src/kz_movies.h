#pragma once

class PS2Runtime;

// Skip intro movies option (killzone.ini [Game] SkipIntro, KZ_SKIP_INTRO=0/1); KZ_MOVIE_LOG=1 logs movie names.
void kzMoviesInstall(PS2Runtime &runtime);
