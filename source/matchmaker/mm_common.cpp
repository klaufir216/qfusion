/*
Copyright (C) 2011 Christian Holmberg

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "../qcommon/qcommon.h"
#include "../qalgo/base64.h"
#include "../qcommon/wswcurl.h"

cvar_t *mm_url;

static bool mm_initialized = false;

// returns static internal string
static const char *MM_PasswordFilename( const char *user ) {
	static char filename[MAX_STRING_CHARS];
	char *user64;

	user64 = (char*)base64_encode( (unsigned char*)user, strlen( user ), NULL );

	Q_snprintfz( filename, sizeof( filename ), "%s.profile", user64 );

	free( user64 );

	return filename;
}

// returns static string of md5 contents
const char *MM_PasswordRead( const char *user ) {
	static char buffer[MAX_STRING_CHARS];
	const char *filename;
	int filenum;
	size_t bytes;

	Com_DPrintf( "MM_PasswordRead %s\n", user );

	filename = MM_PasswordFilename( user );
	if( FS_FOpenFile( filename, &filenum, FS_READ | FS_SECURE ) == -1 ) {
		Com_Printf( "MM_PasswordRead: Couldnt open file %s\n", filename );
		return NULL;
	}

	bytes = FS_Read( buffer, sizeof( buffer ) - 1,  filenum );
	FS_FCloseFile( filenum );

	if( bytes == 0 || bytes >= sizeof( buffer ) - 1 ) {
		return NULL;
	}

	buffer[bytes] = '\0';

	return buffer;
}

void MM_PasswordWrite( const char *user, const char *password ) {
	const char *filename;
	int filenum;

	Com_DPrintf( "MM_PasswordWrite: %s %s\n", user, password );

	filename = MM_PasswordFilename( user );
	if( FS_FOpenFile( filename, &filenum, FS_WRITE | FS_SECURE ) == -1 ) {
		Com_Printf( "MM_PasswordWrite: Failed to open %s for writing\n", filename );
		return;
	}

	FS_Write( password, strlen( password ), filenum );
	FS_FCloseFile( filenum );

	Com_DPrintf( "MM_PasswordWrite: wrote %s\n", filename );
}

void MM_Frame( int ) {
}

void MM_Init() {
	mm_initialized = false;

	mm_url = Cvar_Get( "mm_url", APP_MATCHMAKER_URL, CVAR_ARCHIVE | CVAR_NOSET );

	mm_initialized = true;
}

void MM_Shutdown() {
	mm_url = NULL;
	mm_initialized = false;
}
