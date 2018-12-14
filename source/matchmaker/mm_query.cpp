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
#include "../matchmaker/mm_common.h"
#include "../matchmaker/mm_query.h"

#include "../qalgo/base64.h"
#include "../qcommon/compression.h"

// For error codes
#include <curl/curl.h>
#include <memory>

void QueryObject::RawCallback( wswcurl_req *req, int wswStatus, void *customp ) {
	auto *query = (QueryObject *)customp;

	if( wswStatus >= 0 ) {
		query->HandleHttpSuccess( req );
	} else if( wswStatus == -CURLE_HTTP_RETURNED_ERROR || wswStatus == -CURLE_HTTP_POST_ERROR ) {
		query->HandleHttpFailure( req, wswStatus );
	} else {
		query->HandleOtherFailure( req, wswStatus );
	}

	// Make sure the status has been set
	assert( query->status >= SUCCEEDED );

	query->completionCallback( query );

	if( query->deleteOnCompletion ) {
		DeleteQuery( query );
	}
}

void QueryObject::HandleOtherFailure( wswcurl_req *, int wswStatus ) {
	Com_Printf( "MM Query CURL error `%s`", wswcurl_errorstr( wswStatus ) );
	switch( -wswStatus ) {
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_CONNECT:
		case CURLE_COULDNT_RESOLVE_PROXY:
		case CURLE_SEND_ERROR:
		case CURLE_RECV_ERROR:
			SetStatus( NETWORK_FAILURE );
			break;
		default:
			SetStatus( OTHER_FAILURE );
			break;
	}
}

void QueryObject::HandleHttpFailure( wswcurl_req *req, int status ) {
	const char *error = wswcurl_errorstr( status );
	const char *url = wswcurl_get_effective_url( req );
	Com_Printf( "MM Query HTTP error: `%s`, url `%s`\n", error, url );

	// Get the real HTTP status. The supplied status argument is something else
	// and might be an HTTP code if negated but lets query tested status explicitly.
	const int httpStatus = wswcurl_get_status( req );
	// Statsow returns these error codes if a request is throttled
	// or a transaction serialization error occurs. We should retry in these cases.
	if( httpStatus == 429 || httpStatus == 503 ) {
		SetStatus( EXPLICIT_RETRY );
		return;
	}

	SetStatus( httpStatus < 500 ? MALFORMED_REQUEST : SERVER_FAILURE );
}

void QueryObject::HandleHttpSuccess( wswcurl_req *req ) {
	const char *contentType = wswcurl_get_content_type( req );
	if( !contentType ) {
		SetStatus( SUCCEEDED );
		return;
	}

	if( strcmp( contentType, "application/json" ) != 0 ) {
		// Some calls return plain text ok (do they?)
		if( strcmp( contentType, "text/plain" ) == 0 ) {
			SetStatus( SUCCEEDED );
		}
		Com_Printf( "MM Query error: unexpected content type `%s`", contentType );
		SetStatus( MALFORMED_RESPONSE );
	}

	size_t rawSize;
	wswcurl_getsize( req, &rawSize );
	if( !rawSize ) {
		// The failure is really handled on application logic level
		SetStatus( SUCCEEDED );
		return;
	}

	// read the response string
	rawResponse = (char *)malloc( rawSize + 1 );
	size_t readSize = wswcurl_read( req, rawResponse, rawSize );
	if( readSize != rawSize ) {
		const char *format = "MM Query error: can't read expected %u bytes, got %d instead\n";
		Com_Printf( format, (unsigned)rawSize, (unsigned)readSize );
		// We think it's better than a "network error"
		SetStatus( MALFORMED_RESPONSE );
	} else {
		if( rawSize ) {
			if( !( responseRoot = cJSON_Parse( rawResponse ) ) ) {
				SetStatus( MALFORMED_RESPONSE );
			}
		}
	}

	rawResponse[rawSize] = '\0';
}

QueryObject::QueryObject( const char *url_, const char *iface_ ) {
	assert( url_ );
	if( !iface_ || !*iface_ ) {
		return;
	}

	size_t len = strlen( iface_ );
	this->iface = (char *)malloc( len + 1 );
	memcpy( this->iface, iface_, len + 1 );
}

QueryObject *QueryObject::NewGetQuery( const char *url, const char *iface ) {
	auto *query = new( malloc( sizeof( QueryObject ) ) )QueryObject( url, iface );
	if( *url == '/' ) {
		url++;
	}
	// TODO: Get url from cvar system!
	query->url = (char *)malloc( strlen( mm_url->string ) + strlen( url ) + 3 );
	// ch : lazy code :(
	// add in '/', '?' and '\0' = 3
	strcpy( query->url, mm_url->string );
	strcat( query->url, "/" );
	strcat( query->url, url );
	strcat( query->url, "?" );
	return query;
}

QueryObject *QueryObject::NewPostQuery( const char *url, const char *iface ) {
	auto *query = new( malloc( sizeof( QueryObject ) ) )QueryObject( url, iface );
	// TODO: Get url from cvar system!
	query->req = wswcurl_create( iface, "%s/%s", mm_url->string, ( *url == '/' ) ? url + 1 : url );
	return query;
}

QueryObject::~QueryObject() {
	// close wswcurl and json_in json_out
	if( req ) {
		wswcurl_delete( req );
	}

	cJSON_Delete( requestRoot );
	cJSON_Delete( responseRoot );

	::free( iface );
	::free( url );
	::free( rawResponse );
}

bool QueryObject::SendForStatusPolling() {
	if( !Prepare() ) {
		return false;
	}

	Fire();
	return true;
}

bool QueryObject::SendDeletingOnCompletion( CompletionCallback &&callback ) {
	if( !Prepare() ) {
		return false;
	}

	this->completionCallback = std::move( callback );
	this->deleteOnCompletion = true;

	Fire();
	return true;
}

void QueryObject::Fire() {
	status.store( STARTED, std::memory_order_seq_cst );
	wswcurl_stream_callbacks( req, nullptr, &QueryObject::RawCallback, nullptr, (void*)this );
	wswcurl_start( req );
}

struct CallFree {
	void operator()( void *p ) {
		free( p );
	}
};

bool QueryObject::Prepare() {
	assert( status < STARTED );

	if( !req && !url ) {
		FailWith( "Neither request not url are specified" );
	}

	// GET request, finish the url and create the object
	if( !req ) {
		assert( url );
		req = wswcurl_create( iface, url );
		return true;
	}

	if( !requestRoot ) {
		return true;
	}

	std::unique_ptr<char, CallFree> json_text( cJSON_Print( requestRoot ) );
	size_t jsonSize = strlen( json_text.get() );

	using DataHolder = std::unique_ptr<uint8_t, CallFree>;

	// compress
	unsigned long compSize = (unsigned)( jsonSize * 1.1f ) + 12;
	DataHolder compressed( (uint8_t *)::malloc( compSize ) );
	if( !compressed ) {
		Com_Printf( "StatQuery: Failed to allocate space for compressed JSON\n" );
		return false;
	}

	int z_result = qzcompress( compressed.get(), &compSize, (unsigned char*)json_text.get(), jsonSize );
	if( z_result != Z_OK ) {
		Com_Printf( "StatQuery: Failed to compress JSON\n" );
		return false;
	}

	// base64
	size_t b64Size;
	DataHolder base64Encoded( ::base64_encode( (unsigned char *)compressed.get(), compSize, &b64Size ) );
	if( !base64Encoded ) {
		Com_Printf( "StatQuery: Failed to base64_encode JSON\n" );
		return false;
	}

	// TODO: Save base64 attachment for retries?

	// set the json field to POST request
	wswcurl_formadd_raw( req, "json_attachment", base64Encoded.get(), b64Size );
	return true;
}

void QueryObject::Poll() {
	// TODO: Do that individually for the specified query?
	wswcurl_perform();
}

// Note: this is only for executables that link qcommon code directly.
// If query object is exported to libraries they should define their own implementations.
void QueryObject::FailWith( const char *format, ... ) {
	char buffer[2048];

	va_list va;
	va_start( va, format );
	Q_vsnprintfz( buffer, sizeof( buffer ), format, va );
	va_end( va );

	buffer[sizeof( buffer ) - 1] = '\0';

	Com_Error( ERR_FATAL, "Fatal query error: %s\n", buffer );
}
