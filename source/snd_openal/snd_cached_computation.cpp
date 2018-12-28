#include "snd_cached_computation.h"

#include "../gameshared/q_comref.h"

#include <initializer_list>

void CachedComputation::EnsureValid() {
	const char *actualMap = trap_GetConfigString( CS_WORLDMODEL );
	const int actualNumLeafs = trap_NumLeafs();
	// Should not be used for empty maps
	if( !*actualMap || !actualNumLeafs ) {
		return;
	}

	const char *actualChecksum = trap_GetConfigString( CS_MAPCHECKSUM );
	if( actualNumLeafs == numLeafs && !strcmp( actualMap, mapName ) && !strcmp( actualChecksum, mapHash ) ) {
		return;
	}

	// These values are now expected to be actual at the moment of ResetExistingState() call
	this->numLeafs = actualNumLeafs;
	Q_strncpyz( this->mapName, actualMap, MAX_CONFIGSTRING_CHARS );
	Q_strncpyz( this->mapHash, actualChecksum, MAX_CONFIGSTRING_CHARS );

	isUsingValidData = false;
	ResetExistingState();

	// Try reading from basewsw first.
	// High-quality precomputed data is expected to be shipped within the game distribution.
	// If it was a custom map a user has loaded once, results are expected to be under the cache directory.
	for( int fsFlags : { FS_READ, ( FS_READ | FS_CACHE ) } ) {
		if( TryReadFromFile( fsFlags ) ) {
			isUsingValidData = true;
			CommitUpdate();
			return;
		}
	}

	NotifyOfBeingAboutToCompute();

	const bool fastAndCoarse = !trap_Cvar_Value( "developer" );
	if( !ComputeNewState( fastAndCoarse ) ) {
		NotifyOfComputationFailure();
		ProvideDummyData();
		CommitUpdate();
		return;
	}

	isUsingValidData = true;
	NotifyOfComputationSuccess();

	// Always saves to cache (and not to basewsw)
	if( SaveToCache() ) {
		NotifyOfSerializationSuccess();
	} else {
		NotifyOfSerializationFailure();
	}

	CommitUpdate();
}

void CachedComputation::NotifyOfBeingAboutToCompute() {
	Com_Printf( S_COLOR_YELLOW "Can't load a %s. Computing a new one (this may take a while)\n", logTag );
}

void CachedComputation::NotifyOfComputationSuccess() {
	if( trap_Cvar_Value( "developer" ) ) {
		Com_Printf( S_COLOR_GREY "Computations of new %s have been completed successfully\n", logTag );
	}
}

void CachedComputation::NotifyOfComputationFailure() {
	Com_Printf( S_COLOR_YELLOW "Can't compute a new %s data\n", logTag );
}

void CachedComputation::NotifyOfSerializationSuccess() {
	if( trap_Cvar_Value( "developer" ) ) {
		Com_Printf( S_COLOR_GREY "Computation results for %s have been saved to a file cache successfully\n", logTag );
	}
}

void CachedComputation::NotifyOfSerializationFailure() {
	Com_Printf( S_COLOR_YELLOW "Can't save %s computation results to a file cache\n", logTag );
}

bool CachedComputationReader::ExpectString( const char *string ) {
	SkipWhiteSpace();

	for( const char *s = string; *s; ++s ) {
		if( *s == '\r' || *s == '\n' || *s == ' ' || *s == '\t' ) {
			trap_Error( "CachedComputationReader::ExpectString(): The expected string should not contain a whitespace" );
		}
		// We have to be aware of both CR and LF
		// as this method could be called for binary files too
		if( *dataPtr == '\n' || *dataPtr == '\r' ) {
			return false;
		}
		if( *dataPtr != *s ) {
			return false;
		}
		dataPtr++;
	}

	if( BytesLeft() >= 2 ) {
		// Hack! CachedComputationWriter::WriteString() puts \r\n for every file be it textual or binary.
		// Skip \r\n sequence if it present.
		// This should not lead to a malformed input as binary cache files
		// are not intended to be edited via external tools.
		// TODO: Put integer magics before actual binary data to ensure the data is read correct?
		// Note that every binary file performs data validation
		// to avoid runtime crashes (e.g. due to illegal array offsets).
		if( dataPtr[0] == '\r' && dataPtr[1] == '\n' ) {
			dataPtr += 2;
		}
	}

	return true;
}

bool CachedComputationReader::Read( void *data, size_t size ) {
	if( BytesLeft() < size ) {
		return false;
	}
	memcpy( data, dataPtr, size );
	dataPtr += size;
	return true;
}

CachedComputationReader::CachedComputationReader( const CachedComputation *parent_, int fileFlags, bool textMode )
	: CachedComputationIOHelper( parent_, fileFlags ) {
	if( fsResult < 0 ) {
		return;
	}

	fileSize = fsResult;

	fileData = (char *)::S_Malloc( (size_t)( fileSize + 1u ) );
	if( !fileData ) {
		fsResult = -1;
		return;
	}

	assert( fd >= 0 );
	if( fsResult != trap_FS_Read( fileData, (size_t)fileSize, fd ) ) {
		fsResult = -1;
		return;
	}

	// Terminate by \0 immediately after the file has been read (for ExpectString())
	// TODO: Read only amount of bytes expected by all ExpectString() calls first?
	// This would be complicated since we have allowed whitespaces
	// at the beginning of the file/between header strings.
	// Note: it is intended that cache faults are very rare
	// so reading entire files should not be a significant impact on performance.
	fileData[fileSize] = '\0';
	dataPtr = fileData;

	if( !ExpectString( Version() ) || !ExpectString( MapName() ) || !ExpectString( MapHash() ) ) {
		fsResult = -1;
		return;
	}

	if( !textMode ) {
		return;
	}

	// Convert all \r to \n to simplify further lines parsing
	for( char *p = dataPtr; p != fileData + fileSize; ++p ) {
		if( *p == '\r' ) {
			*p = '\n';
		}
	}
}

CachedComputationWriter::CachedComputationWriter( const CachedComputation *parent_ )
	: CachedComputationIOHelper( parent_, ( FS_WRITE | FS_CACHE ) ) {
	if( fsResult < 0 ) {
		return;
	}
	if( !WriteString( Version() ) || !WriteString( MapName() ) || !WriteString( MapHash() ) ) {
		fsResult = -1;
	}
}

bool CachedComputationWriter::WriteString( const char *string ) {
	char buffer[MAX_STRING_CHARS];
	auto charsPrinted = (unsigned)Q_snprintfz( buffer, sizeof( buffer ), "%s\r\n", string );
	if( charsPrinted == (unsigned)trap_FS_Write( buffer, charsPrinted, fd ) ) {
		bytesWritten += charsPrinted;
		return true;
	}
	return false;
}

bool CachedComputationWriter::Write( const void *data, size_t size ) {
	if( fsResult < 0 ) {
		return false;
	}
	if( trap_FS_Write( data, size, fd ) != (int)size ) {
		fsResult = -1;
		return false;
	}
	bytesWritten += size;
	return true;
}