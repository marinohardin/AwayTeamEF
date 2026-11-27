// Current version of the single player game

// desktop: the app's version, worded as the engine's (qcommon/q_shared.h in the engine);
// EF_APP_VERSION comes from efgame/CMakeLists.txt
#if defined( EF_APP_VERSION )
#if defined( __APPLE__ )
#define	Q3_VERSION		"AwayTeamEF for Mac " EF_APP_VERSION
#elif defined( _WIN32 )
#define	Q3_VERSION		"AwayTeamEF for Windows " EF_APP_VERSION
#else
#define	Q3_VERSION		"AwayTeamEF for Linux " EF_APP_VERSION
#endif
#else
#define	Q3_VERSION		"Elite Reinforce v0.7"
#endif

// end
