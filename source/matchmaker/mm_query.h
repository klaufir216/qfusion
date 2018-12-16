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

#ifndef __MM_QUERY_H__
#define __MM_QUERY_H__

#include "mm_rating.h"

#include "../qcommon/wswcurl.h"
#include "../qcommon/cjson.h"

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <functional>
#include <atomic>

/**
 * A proxy that wraps an underlying {@code cJSON} value and provides convenient accessor methods.
 */
class NodeReader {
protected:
	const cJSON *const underlying;

	explicit NodeReader( const cJSON *underlying_ ): underlying( underlying_ ) {}

	static const char *AsString( const cJSON *node, const char *nameAsField = nullptr );
	static double AsDouble( const cJSON *node, const char *nameAsField = nullptr );
};

/**
 * A wrapper over a raw {@code cJSON} object value that allows retrieval of fields by name.
 */
class ObjectReader: public NodeReader {
	friend class ArrayReader;
public:
	explicit ObjectReader( const cJSON *underlying_ ): NodeReader( underlying_ ) {
		assert( underlying_->type == cJSON_Object );
	}

	const char *GetString( const char *field, const char *defaultValue = nullptr ) const;
	double GetDouble( const char *field, double defaultValue = std::numeric_limits<double>::quiet_NaN() ) const;

	cJSON *GetObject( const char *field ) const;
	cJSON *GetArray( const char *field ) const;
};

/**
 * A wrapper over a raw {@code cJSON} object value that allows sequential iteration and retrieval of elements.
 */
class ArrayReader: public NodeReader {
	friend class ObjectReader;

	const cJSON *child;
public:
	explicit ArrayReader( const cJSON *underlying_ ): NodeReader( underlying_ ) {
		assert( underlying_->type == cJSON_Object );
		child = underlying_->child;
	}

	bool IsDone() const { return child; }

	void Next() {
		assert( IsDone() );
		child = child->next;
	}

	bool IsAtArray() const { return child && child->type == cJSON_Array; }
	ArrayReader GetChildArray() const { return ArrayReader( child ); }
	bool IsAtObject() const { return child && child->type == cJSON_Object; }
	ObjectReader GetChildObject() const { return ObjectReader( child ); }
};

/**
 * An object that helps managing a query lifecycle including
 * setting predefined form parameters and arbitrary JSON attachments,
 * starting execution of a query, checking a query current status and result retrieval.
 * @note most of methods are defined inline and hence available at every inclusion site.
 * Some methods are way too tied with {@code qcommon} stuff and have to be exported in modules (namely the game module).
 * The {@code FailWith} call should be defined in modules appropriately as well.
 */
class QueryObject {
	friend class JsonWriter;
	friend class NodeReader;
	friend class ObjectReader;
	friend class ArrayReader;
public:
	using CompletionCallback = std::function<void( QueryObject * )>;

protected:
	// An implementation of this is left for every binary it gets included.
#ifndef _MSC_VER
	[[noreturn]]
	static void FailWith( const char *format, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
#else
	[[noreturn]]
	static void FailWith( _Printf_format_string_ const char *format, ... );
#endif
private:
	CompletionCallback completionCallback = []( QueryObject * ) {};
	cJSON *requestRoot { nullptr };
	cJSON *responseRoot { nullptr };

	wswcurl_req_s *req { nullptr };
	/**
	 * Just copy a request to this var and it is going to be deleted
	 * (we do not want to link against {@code wswcurl} in the game module.
	 * This is for supporting query restart functionality.
	 */
	wswcurl_req_s *oldReq { nullptr };

	char *url { nullptr };
	size_t urlLen { 0 };
	char *iface { nullptr };
	char *rawResponse { nullptr };

	/**
	 * Maintain a copy of parameters for query restarts
	 */
	struct FormParam {
		FormParam *next { nullptr };
		const char *name { nullptr };
		const char *value { nullptr };
	};

	FormParam *formParamsHead { nullptr };
	uint8_t *base64EncodedJson { nullptr };
	size_t encodedJsonDataSize { 0 };

	enum Status: uint32_t {
		CREATED,
		STARTED,
		SUCCEEDED,
		OTHER_FAILURE,
		NETWORK_FAILURE,
		SERVER_FAILURE,
		MALFORMED_REQUEST,
		MALFORMED_RESPONSE,
		EXPLICIT_RETRY
	};

	std::atomic<Status> status { CREATED };

	bool isPostQuery { false };
	bool deleteOnCompletion { false };

	QueryObject( const char *url_, const char *iface_ );

	~QueryObject();
public:
	static QueryObject *NewGetQuery( const char *url, const char *iface = nullptr );
	static QueryObject *NewPostQuery( const char *url, const char *iface = nullptr );

	static void DeleteQuery( QueryObject *query ) {
		query->~QueryObject();
		::free( query );
	}

	bool Prepare();

	bool ConvertJsonToEncodedForm();

	static void RawCallback( wswcurl_req *req, int wswStatus, void *customp );

	void HandleOtherFailure( wswcurl_req *req, int wswStatus );

	void HandleHttpFailure( wswcurl_req *req, int wswStatus );

	/**
	 * Should handle situations when {@code RawCallback()} has been called with non-negative status.
	 * An attempt to get and parse response body should be made.
	 * {@code this->status} should be set appropriately.
	 */
	void HandleHttpSuccess( wswcurl_req *req );

	void SetStatus( Status status_ ) {
		this->status.store( status_, std::memory_order_relaxed );
	}

	/**
	 * Set a key/value request parameter.
	 * GET parameters are added to an URL immediately.
	 * POST parameters are saved and will be added as form parameters.
	 * @param name a field name
	 * @param value a field value
	 * @return this object for conformance to fluent API style.
	 * @note this method should be defined in this header within the class definition
	 * so its binary code is available for all usage sites (executables and game module).
	 */
	QueryObject &SetField( const char *name, const char *value ) {
		assert( name && value );
		size_t nameDataSize = ::strlen( name ) + 1;
		assert( nameDataSize < std::numeric_limits<uint32_t>::max() );
		size_t valueDataSize = ::strlen( value ) + 1;
		assert( valueDataSize < std::numeric_limits<uint32_t>::max() );

		if( !isPostQuery ) {
			assert( url );
			// GET request, store parameters
			// add in '=', '&' and '\0' = 3

			// FIXME: add proper URL encode
			size_t len = urlLen + 1 + nameDataSize + valueDataSize;
			url = (char *)realloc( url, len );
			urlLen = len;
			strcat( url, name );
			strcat( url, "=" );
			strcat( url, value );
			strcat( url, "&" );
			return *this;
		}

		auto *mem = (uint8_t *) ::malloc( sizeof( FormParam ) + nameDataSize + valueDataSize );
		auto *param = new( mem )FormParam;
		mem += sizeof( FormParam );
		param->name = (char *) (mem);
		::memcpy( mem, name, nameDataSize );
		mem += nameDataSize;
		param->value = (char *) (mem);
		::memcpy( mem, value, valueDataSize );
		param->next = formParamsHead;
		formParamsHead = param;
		return *this;
	}

	QueryObject &SetField( const char *name, const mm_uuid_t &value ) {
		char buffer[UUID_BUFFER_SIZE];
		return SetField( name, value.ToString( buffer ) );
	}
public:
	/**
	 * Reads a root response string field. Provided for convenience.
	 * @param field the field name
	 * @param defaultValue a default value for an absent field (should be non-null)
	 * @return a string value of the read field or the supplied default value
	 * @note the field must have a string type if present.
	 */
	const char *GetRootString( const char *field, const char *defaultValue ) const {
		return ObjectReader( ResponseJsonRoot() ).GetString( field, defaultValue );
	}

	/**
	 * Reads a root response numeric field. Provided for convenience.
	 * @param field the field name
	 * @param defaultValue a default value for an absent field (should not be a NAN).
	 * @return a numeric value of the read field or the supplied default value.
	 * @note the field must have a numeric type if present.
	 */
	double GetRootDouble( const char *field, double defaultValue ) {
		return ObjectReader( ResponseJsonRoot() ).GetDouble( field, defaultValue );
	}

	QueryObject &SetServerSession( const char *value ) {
		return SetField( "server_session", value );
	}

	QueryObject &SetServerSession( const mm_uuid_t &value ) {
		return SetField( "server_session", value );
	}

	QueryObject &SetClientSession( const char *value ) {
		return SetField( "client_session", value );
	}

	QueryObject &SetClientSession( const mm_uuid_t &value ) {
		return SetField( "client_session", value );
	}

	QueryObject &SetTicket( const mm_uuid_t &value ) {
		return SetField( "ticket", value );
	}

	QueryObject &SetHandle( const mm_uuid_t &value ) {
		return SetField( "handle", value );
	}

	QueryObject &SetLogin( const char *value ) {
		return SetField( "login", value );
	}

	QueryObject &SetPassword( const char *value ) {
		return SetField( "password", value );
	}

	QueryObject &SetPort( const char *value ) {
		return SetField( "port", value );
	}

	QueryObject &SetAuthKey( const char *value ) {
		return SetField( "auth_key", value );
	}

	QueryObject &SetServerName( const char *value ) {
		return SetField( "server_name", value );
	}

	QueryObject &SetDemosBaseUrl( const char *value ) {
		return SetField( "demos_baseurl", value );
	}

	QueryObject &SetServerAddress( const char *value ) {
		return SetField( "server_address", value );
	}

	QueryObject &SetClientAddress( const char *value ) {
		return SetField( "client_address", value );
	}

	cJSON *RequestJsonRoot() {
		if( !isPostQuery ) {
			FailWith( "Attempt to add a JSON root to a GET request" );
		}
		if( status.load( std::memory_order_seq_cst ) >= STARTED ) {
			FailWith( "Attempt to add a JSON root to an already started request" );
		}
		if( base64EncodedJson ) {
			FailWith( "Attempt to modify already built JSON data (saved as base64)" );
		}
		if( !requestRoot ) {
			requestRoot = cJSON_CreateObject();
		}
		return requestRoot;
	}

	void CheckStatusOnGet( const char *itemToGet ) const {
		Status actualStatus = this->status.load( std::memory_order_seq_cst );
		if( actualStatus < SUCCEEDED ) {
			FailWith( "Attempt to get %s while the request is not ready yet", itemToGet );
		} else if( actualStatus > SUCCEEDED ) {
			FailWith( "Attempt to get %s while the request has failed", itemToGet );
		}
	}

	const cJSON *ResponseJsonRoot() const {
		CheckStatusOnGet( "a JSON response root" );
		return responseRoot;
	}

	cJSON *ResponseJsonRoot() {
		CheckStatusOnGet( "a JSON response root" );
		return responseRoot;
	}

	const char *RawResponse() {
		CheckStatusOnGet( "a raw response" );
		return rawResponse;
	}

	void Fire();

	bool SendForStatusPolling();

	bool SendDeletingOnCompletion( CompletionCallback &&callback );

	static void Poll();

	bool IsReady() const {
		return status.load( std::memory_order_seq_cst ) >= SUCCEEDED;
	}

	Status GetCompletionStatus() const {
		Status actualStatus = status.load( std::memory_order_seq_cst );
		if( actualStatus < SUCCEEDED ) {
			FailWith( "Attempt to test status of request that is not ready yet" );
		}
		return actualStatus;
	}

	bool TestStatus( Status testedStatus ) const {
		return GetCompletionStatus() == testedStatus;
	}

	bool HasSucceeded() const { return TestStatus( SUCCEEDED ); }
	bool IsOtherFailure() const { return TestStatus( OTHER_FAILURE ); }
	bool IsNetworkFailure() const { return TestStatus( NETWORK_FAILURE ); }
	bool IsServerFailure() const { return TestStatus( SERVER_FAILURE ); }
	bool WasRequestMalformed() const { return TestStatus( MALFORMED_REQUEST ); }
	bool WasResponseMalformed() const { return TestStatus( MALFORMED_RESPONSE ); }
	bool ServerToldToRetry() const { return TestStatus( EXPLICIT_RETRY ); }

	bool ShouldRetry() {
		Status status_ = GetCompletionStatus();
		return status_ == EXPLICIT_RETRY || status_ == NETWORK_FAILURE || status_ == SERVER_FAILURE;
	}

	void ResetForRetry() {
		if( req ) {
			assert( !oldReq );
			std::swap( req, oldReq );
		}
		if( rawResponse ) {
			::free( rawResponse );
		}
		SetStatus( CREATED );
	}
};

inline const char* NodeReader::AsString( const cJSON *node, const char *nameAsField ) {
	if( node && node->type == cJSON_String && node->valuestring ) {
		return node->valuestring;
	}
	if( nameAsField ) {
		QueryObject::FailWith( "Can't get a string value of `%s`", nameAsField );
	}
	QueryObject::FailWith( "Can't get a string value of an array element" );
}

inline double NodeReader::AsDouble( const cJSON *node, const char *nameAsField ) {
	if( node && node->type == cJSON_Number ) {
		return node->valuedouble;
	}
	if( nameAsField ) {
		QueryObject::FailWith( "Can't get a double value of `%s`", nameAsField );
	}
	QueryObject::FailWith( "Can't get a double value of an array element" );
}

inline const char *ObjectReader::GetString( const char *field, const char *defaultValue ) const {
	cJSON *f = cJSON_GetObjectItem( const_cast<cJSON *>( underlying ), field );
	if( !f ) {
		if( defaultValue ) {
			return defaultValue;
		}
		QueryObject::FailWith( "Can't get `%s` field\n", field );
	}
	return AsString( f, field );
}

inline cJSON *ObjectReader::GetObject( const char *field ) const {
	cJSON *f = cJSON_GetObjectItem( const_cast<cJSON *>( underlying ), field );
	return ( f && f->type == cJSON_Object ) ? f : nullptr;
}

inline cJSON *ObjectReader::GetArray( const char *field ) const {
	cJSON *f = cJSON_GetObjectItem( const_cast<cJSON *>( underlying ), field );
	return ( f && f->type == cJSON_Array ) ? f : nullptr;
}

inline double ObjectReader::GetDouble( const char *field, double defaultValue ) const {
	cJSON *f = cJSON_GetObjectItem( const_cast<cJSON *>( underlying ), field );
	if( !f ) {
		if( !std::isnan( defaultValue ) ) {
			return defaultValue;
		}
		QueryObject::FailWith( "Can't get `%s` field\n", field );
	}
	return AsDouble( f, field );
}

class alignas( 8 )JsonWriter {
	friend class CompoundWriter;
	friend class ObjectWriter;
	friend class ArrayWriter;
	friend struct WritersAllocator;

	static constexpr int STACK_SIZE = 32;

	static bool CheckTopOfStack( const char *tag, int topOfStack_ ) {
		if( topOfStack_ < 0 || topOfStack_ >= STACK_SIZE ) {
			const char *kind = topOfStack_ < 0 ? "underflow" : "overflow";
			QueryObject::FailWith( "%s: Objects stack %s, top of stack index is %d\n", tag, kind, topOfStack_ );
		}
		return true;
	}

	void AddSection( const char *name, cJSON *section ) {
		cJSON *attachTo = TopOfStack().section;
		assert( attachTo->type == cJSON_Object || attachTo->type == cJSON_Array );
		if( attachTo->type == cJSON_Object ) {
			cJSON_AddItemToObject( attachTo, name, section );
		} else {
			cJSON_AddItemToArray( attachTo, section );
		}
	}

	void NotifyOfNewArray( const char *name ) {
		cJSON *section = cJSON_CreateArray();
		AddSection( name, section );
		topOfStackIndex++;
		stack[topOfStackIndex] = writersAllocator.NewArrayWriter( section );
	}

	void NotifyOfNewObject( const char *name ) {
		cJSON *section = cJSON_CreateObject();
		AddSection( name, section );
		topOfStackIndex++;
		stack[topOfStackIndex] = writersAllocator.NewObjectWriter( section );
	}

	void NotifyOfArrayEnd() {
		writersAllocator.DeleteHelper( &TopOfStack() );
		topOfStackIndex--;
	}

	void NotifyOfObjectEnd() {
		writersAllocator.DeleteHelper( &TopOfStack() );
		topOfStackIndex--;
	}

	/**
	 * An object that can be on top of the stack and that
	 * actually attaches values to the current top JSON node.
	 */
	class CompoundWriter {
		friend class JsonWriter;
	protected:
		JsonWriter *const parent;
		cJSON *const section;

		int64_t CheckPrecisionLoss( int64_t value ) {
			// Try to prevent optimizing out this
			volatile double dValue = value;
			if( (volatile int64_t)dValue != value ) {
				QueryObject::FailWith( "Can't store %" PRIi64 " in double without precision loss", value );
			}
			return value;
		}
	public:
		CompoundWriter( JsonWriter *parent_, cJSON *section_ )
			: parent( parent_ ), section( section_ ) {}

		virtual	~CompoundWriter() = default;

		virtual void operator<<( const char *nameOrValue ) = 0;
		virtual void operator<<( int value ) = 0;
		virtual void operator<<( int64_t value ) = 0;
		virtual void operator<<( double value ) = 0;
		virtual void operator<<( const mm_uuid_t &value ) = 0;
		virtual void operator<<( char ch ) = 0;
	};

	/**
	 * A {@code CompoundWriter} that attaches values to the current top JSON object node.
	 */
	class ObjectWriter: public CompoundWriter {
		const char *fieldName;

		const char *CheckFieldName( const char *tag ) {
			if( !fieldName ) {
				QueryObject::FailWith( "JsonWriter::ObjectWriter::operator<<(%s): "
				    "A field name has not been set before supplying a value", tag );
			}
			return fieldName;
		}
	public:
		ObjectWriter( JsonWriter *parent_, cJSON *section_ )
			: CompoundWriter( parent_, section_ ), fieldName( nullptr ) {
			assert( section_->type == cJSON_Object );
		}

		void operator<<( const char *nameOrValue ) override {
			if( !fieldName ) {
				// TODO: Check whether it is a valid identifier?
				fieldName = nameOrValue;
			} else {
				cJSON_AddStringToObject( section, fieldName, nameOrValue );
				fieldName = nullptr;
			}
		}

		void operator<<( int value ) override {
			cJSON_AddNumberToObject( section, CheckFieldName( "int" ), value );
			fieldName = nullptr;
		}

		void operator<<( int64_t value ) override {
			cJSON_AddNumberToObject( section, CheckFieldName( "int64_t"), CheckPrecisionLoss( value ) );
			fieldName = nullptr;
		}

		void operator<<( double value ) override {
			cJSON_AddNumberToObject( section, CheckFieldName( "double" ), value );
			fieldName = nullptr;
		}

		void operator<<( const mm_uuid_t &value ) override {
			char buffer[UUID_BUFFER_SIZE];
			value.ToString( buffer );
			cJSON_AddStringToObject( section, CheckFieldName( "const mm_uuid_t &" ), buffer );
			fieldName = nullptr;
		}

		/**
		 * Starts a new array/object or ends a current one if valid characters are supplied
		 */
		void operator<<( char ch ) override {
			if( ch == '{' ) {
				parent->NotifyOfNewObject( CheckFieldName( "{..." ) );
				fieldName = nullptr;
			} else if( ch == '[' ) {
				parent->NotifyOfNewArray( CheckFieldName( "[..." ) );
				fieldName = nullptr;
			} else if( ch == '}' ) {
				parent->NotifyOfObjectEnd();
			} else if( ch == ']' ) {
				QueryObject::FailWith( "ArrayWriter::operator<<('...]'): Unexpected token (an array end token)" );
			} else {
				QueryObject::FailWith( "ArrayWriter::operator<<(char): Illegal character (%d as an integer)", (int)ch );
			}
		}
	};

	/**
	 * A {@code CompoundWriter} that attaches values to the current top JSON array node.
	 */
	class ArrayWriter: public CompoundWriter {
	public:
		ArrayWriter( JsonWriter *parent_, cJSON *section_ )
			: CompoundWriter( parent_, section_ ) {
			assert( section_->type == cJSON_Array );
		}

		void operator<<( const char *nameOrValue ) override {
			cJSON_AddItemToArray( section, cJSON_CreateString( nameOrValue ) );
		}

		void operator<<( int value ) override {
			cJSON_AddItemToArray( section, cJSON_CreateNumber( value ) );
		}

		void operator<<( int64_t value ) override {
			cJSON_AddItemToArray( section, cJSON_CreateNumber( CheckPrecisionLoss( value ) ) );
		}

		void operator<<( double value ) override {
			cJSON_AddItemToArray( section, cJSON_CreateNumber( value ) );
		}

		void operator<<( const mm_uuid_t &value ) override {
			char buffer[UUID_BUFFER_SIZE];
			cJSON_AddItemToArray( section, cJSON_CreateString( value.ToString( buffer ) ) );
		}

		/**
		 * Starts a new array/object or ends a current one if valid characters are supplied.
		 */
		void operator<<( char ch ) override {
			if( ch == '[' ) {
				parent->NotifyOfNewArray( nullptr );
			} else if( ch == '{' ) {
				parent->NotifyOfNewObject( nullptr );
			} else if( ch == ']' ) {
				parent->NotifyOfArrayEnd();
			} else if( ch == '}' ) {
				QueryObject::FailWith( "ArrayWriter::operator<<('...}'): Unexpected token (an object end token)" );
			} else {
				QueryObject::FailWith( "ArrayWriter::operator<<(char): Illegal character (%d as an integer)", (int)ch );
			}
		}
	};

	class alignas( 8 )StackedWritersAllocator {
	protected:
		static_assert( sizeof( ObjectWriter ) >= sizeof( ArrayWriter ), "Redefine LargestEntry" );
		using LargestEntry = ObjectWriter;

		static constexpr auto ENTRY_SIZE = ( sizeof( LargestEntry ) % 8 ) ?
			( sizeof( LargestEntry ) + 8 - sizeof( LargestEntry ) % 8 ) : sizeof( LargestEntry );

		alignas( 8 ) uint8_t storage[STACK_SIZE * ENTRY_SIZE];

		JsonWriter *parent;
		int topOfStack;

		void *AllocEntry( const char *tag ) {
			if( CheckTopOfStack( tag, topOfStack ) ) {
				return storage + ( topOfStack++ ) * ENTRY_SIZE;
			}
			return nullptr;
		}
	public:
		explicit StackedWritersAllocator( JsonWriter *parent_ )
			: parent( parent_ ), topOfStack( 0 ) {
			if( ( (uintptr_t)this ) % 8 ) {
				QueryObject::FailWith( "StackedHelpersAllocator(): the object is misaligned!\n" );
			}
		}

		ArrayWriter *NewArrayWriter( cJSON *section ) {
			return new( AllocEntry( "array" ) )ArrayWriter( parent, section );
		}

		ObjectWriter *NewObjectWriter( cJSON *section ) {
			return new( AllocEntry( "object" ) )ObjectWriter( parent, section );
		}

		void DeleteHelper( CompoundWriter *writer ) {
			writer->~CompoundWriter();
			if( (uint8_t *)writer != storage + ( topOfStack - 1 ) * ENTRY_SIZE ) {
				QueryObject::FailWith( "WritersAllocator::DeleteWriter(): "
									   "Attempt to delete an entry that is not on top of stack\n" );
			}
			topOfStack--;
		}
	};

	cJSON *root;

	StackedWritersAllocator writersAllocator;

	// Put the root object onto the top of stack
	// Do not require closing it explicitly
	CompoundWriter *stack[32 + 1];
	int topOfStackIndex { 0 };

	CompoundWriter &TopOfStack() {
		CheckTopOfStack( "JsonWriter::TopOfStack()", topOfStackIndex );
		return *stack[topOfStackIndex];
	}
public:
	explicit JsonWriter( cJSON *root_ )
		: root( root_ ), writersAllocator( this ) {
		stack[topOfStackIndex] = writersAllocator.NewObjectWriter( root );
	}

	/**
	 * Submits a string to the query writer.
	 * If the currently written child is an object and no field name set yet the string is treated as a new field name.
	 * In all other cases the string is treated as a value of an object field or an array element.
	 * @param nameOrValue a name of a field or a string value.
	 */
	JsonWriter &operator<<( const char *nameOrValue ) {
		TopOfStack() << nameOrValue;
		return *this;
	}

	JsonWriter &operator<<( int value ) {
		TopOfStack() << value;
		return *this;
	}

	JsonWriter &operator<<( int64_t value ) {
		TopOfStack() << value;
		return *this;
	}

	JsonWriter &operator<<( double value ) {
		TopOfStack() << value;
		return *this;
	}

	JsonWriter &operator<<( const mm_uuid_t &value ) {
		TopOfStack() << value;
		return *this;
	}

	JsonWriter &operator<<( char ch ) {
		TopOfStack() << ch;
		return *this;
	}
};

#endif
