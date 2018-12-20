#include "../qcommon/qcommon.h"
#include "mm_reports_storage.h"
#include "../qalgo/SingletonHolder.h"

#include "../../third-party/sqlite-amalgamation/sqlite3.h"

#include <algorithm>
#include <initializer_list>
#include <functional>

/**
 * Allows to tie a connection lifecycle to a lexical scope.
 */
struct ScopedConnectionGuard {
	LocalReportsStorage *const parent;
	DbConnection connection;

	/**
	 * Accepts already (maybe) existing connection.
	 */
	ScopedConnectionGuard( LocalReportsStorage *parent_, DbConnection connection_ )
		: parent( parent_ ), connection( connection_ ) {}

	/**
	 * Requests the {@code LocalReportStorage} parent to create a connection.
	 */
	explicit ScopedConnectionGuard( LocalReportsStorage *parent_ )
		: parent( parent_ ), connection( parent_->NewConnection() ) {}

	/**
	 * Allows passing this object as a {@code DbConnection} without explicit casts.
	 */
	operator DbConnection() {
		return connection;
	}

	/**
	 * Call this if the held connection should be closed forcefully right now for some reasons.
	 */
	void ForceClosing() {
		if( connection ) {
			(void)sqlite3_close( connection );
			connection = nullptr;
		}
	}

	~ScopedConnectionGuard() {
		if( connection ) {
			parent->DeleteConnection( connection );
		}
	}
};

/**
 * A base class for wrappers over raw connections.
 */
class SQLiteAdapter {
protected:
	DbConnection const connection;

	explicit SQLiteAdapter( DbConnection connection_ ): connection( connection_ ) {}
};

/**
 * Provides an interface for execution of SQL statements in immediate mode.
 */
class SQLiteExecAdapter: public SQLiteAdapter {
	bool ExecImpl( const char *sql );
	void ExecOrFailImpl( const char *sql );
public:
	explicit SQLiteExecAdapter( DbConnection connection ) : SQLiteAdapter( connection ) {}

	bool Exec( const char *sql ) { return ExecImpl( sql ); }
	bool ExecV( const char *sql, ... );
	void ExecOrFail( const char *sql ) { ExecOrFailImpl( sql ); }
	void ExecOrFailV( const char *format, ... );

	bool Begin() {
		// Some notes...
		// Override the default "deferred" isolation level that is very weak.
		// Use the most strict mode possible.
		return Exec( "begin exclusive" );
	}

	bool Commit() { return Exec( "commit" ); }
	bool Rollback() { return Exec( "rollback" ); }
};

bool SQLiteExecAdapter::ExecImpl( const char *sql ) {
	char *err = nullptr;
	const int code = ::sqlite3_exec( connection, sql, nullptr, nullptr, &err );
	if( code == SQLITE_OK || code == SQLITE_DONE ) {
		return true;
	}

	if( !err ) {
		return false;
	}

	Com_Printf( S_COLOR_RED "SQLite error: %s\n", err );
	::sqlite3_free( err );
	return false;
}

void SQLiteExecAdapter::ExecOrFailImpl( const char *sql ) {
	char *err = nullptr;
	if( ::sqlite3_exec( connection, sql, nullptr, nullptr, &err ) == SQLITE_OK ) {
		return;
	}

	if( !err ) {
		Com_Error( ERR_FATAL, "An error occurred while executing `%s`\n", sql );
	}

	char buffer[1024];
	Q_snprintfz( buffer, sizeof( buffer ), "An error occurred while executing `%s`: `%s`", sql, err );
	::sqlite3_free( err );
	Com_Error( ERR_FATAL, "%s\n", buffer );
}

bool SQLiteExecAdapter::ExecV( const char *format, ... ) {
	char buffer[2048];
	va_list va;
	va_start( va, format );
	// TODO: Check retval
	Q_vsnprintfz( buffer, sizeof( buffer ), format, va );
	va_end( va );
	return ExecImpl( buffer );
}

void SQLiteExecAdapter::ExecOrFailV( const char *format, ... ) {
	char buffer[2048];
	va_list va;
	va_start( va, format );
	// TODO: Check retval
	Q_vsnprintfz( buffer, sizeof( buffer ), format, va );
	va_end( va );
	ExecOrFailImpl( buffer );
}

// Unfortunately we still have to limit ourselves to C++14
// Just temporarily copy-paste this string_view stub from g_mm.cpp.
class string_view {
	const char *s;
	const size_t len;
public:
	string_view( const char *s_ ) noexcept : s( s_ ), len( strlen( s ) ) {}
	string_view( const char *s_, size_t len_ ): s( s_ ), len( len_ ) {}
	const char *data() const { return s; }
	size_t size() const { return len; }
};

template <typename T> bool SQLiteBindArg( sqlite3_stmt *stmt, int index, const T &value ) {
	return T::implement_specialization_for_this_type();
}

template <> bool SQLiteBindArg( sqlite3_stmt *stmt, int index, const string_view &value ) {
	const int code = ::sqlite3_bind_text( stmt, index, value.data(), (int)value.size(), SQLITE_STATIC );
	if( code == SQLITE_OK ) {
		return true;
	}

	const char *format = S_COLOR_RED "A binding of arg #%d `%s`(@%d #chars) failed with `%s`\n";
	Com_Printf( format, index, value, (int)value.size(), ::sqlite3_errstr( code ) );
	return false;
}

/**
 * Defines a helper for insertion of multiple rows sequentially
 * given a statement for instertion of a single row.
 */
class SQLiteInsertAdapter : public SQLiteAdapter {
	sqlite3_stmt *stmt { nullptr };
public:
	SQLiteInsertAdapter( DbConnection connection_, const char *sql_ )
		: SQLiteAdapter( connection_ ) {
		const int code = ::sqlite3_prepare_v2( connection_, sql_, -1, &stmt, nullptr );
		if( code == SQLITE_OK ) {
			return;
		}

		const char *format = S_COLOR_RED "SQLiteInsertAdapter(): Can't prepare `%s` statement: `%s`\n";
		Com_Printf( format, sql_, ::sqlite3_errstr( code ) );
	}

	~SQLiteInsertAdapter() {
		if( !stmt ) {
			return;
		}

		const int code = ::sqlite3_finalize( stmt );
		if( code == SQLITE_OK ) {
			return;
		}

		const char *format = S_COLOR_RED "~SQLiteInsertAdapter(): a statement finalization returned with `%s`\n";
		Com_Printf( format, ::sqlite3_errstr( code ) );
	}

	// OK lets just add a specialization for the actually used call singature...
	// Variadic templates are still horrible for real use...
	template <typename Arg0, typename Arg1, typename Arg2>
	bool InsertNextRow( Arg0 arg0, Arg1 arg1, Arg2 arg2 ) {
		if( !stmt ) {
			return false;
		}

		const char *tag = "SQLiteInsertHelper::InsertNextRow()";

		if( !SQLiteBindArg( stmt, 0, arg0 ) ) return false;
		if( !SQLiteBindArg( stmt, 1, arg1 ) ) return false;
		if( !SQLiteBindArg( stmt, 2, arg2 ) ) return false;

		int code;
		if( ( code = ::sqlite3_step( stmt ) ) != SQLITE_DONE ) {
			Com_Printf( S_COLOR_RED "%s: An insertion step returned with `%s`\n", tag, ::sqlite3_errstr( code ) );
			return false;
		}

		if( ( code = ::sqlite3_reset( stmt ) ) != SQLITE_OK ) {
			Com_Printf( S_COLOR_YELLOW "%s: A statement reset returned with `%s`\n", tag, ::sqlite3_errstr( code ) );
		}
		if( ( code = ::sqlite3_clear_bindings( stmt ) ) != SQLITE_OK ) {
			Com_Printf( S_COLOR_YELLOW "%s: A bindings clearing returned with `%s`\n", tag, ::sqlite3_errstr( code ) );
		}
		return true;
	}
};

/**
 * A helper for data retrieval from a raw SQLite data rows.
 * Only minimal methods set for our needs is provided.
 */
class SQLiteRowReader {
	friend class SQLiteSelectAdapter;

	sqlite3_stmt *stmt;
	explicit SQLiteRowReader( sqlite3_stmt *stmt_ ): stmt( stmt_ ) {}
public:
	int NumColumns() const {
		return ::sqlite3_data_count( stmt );
	}

	const string_view GetString( int num ) const {
		assert( (unsigned)num < (unsigned)NumColumns() );
		auto *data = (const char *)::sqlite3_column_text( stmt, num );
		assert( data && "Nullable columns are not supported\n" );
		int numBytes = ::sqlite3_column_bytes( stmt, num );
		return string_view( data, (size_t)numBytes );
	}
};

/**
 * A helper for retrieval of rows produced by a SELECT query.
 */
class SQLiteSelectAdapter : public SQLiteAdapter {
	sqlite3_stmt *stmt { nullptr };
public:
	using RowConsumer = std::function<bool(const SQLiteRowReader &)>;

	SQLiteSelectAdapter( DbConnection connection_, const char *sql_ )
		: SQLiteAdapter( connection_ ) {
		const int code = sqlite3_prepare_v2( connection, sql_, -1, &stmt, nullptr );
		if( code == SQLITE_OK ) {
			return;
		}

		const char *format = S_COLOR_RED "SQLiteSelectAdapter(): Can't prepare `%s` statement: `%s`\n";
		Com_Printf( format, sql_, ::sqlite3_errstr( code ) );
	}

	~SQLiteSelectAdapter() {
		if( stmt ) {
			::sqlite3_finalize( stmt );
		}
	}

	/**
	 * Executes a query step and applies the {@code rowConsumer} if needed.
	 * @param rowConsumer a {@code RowConsumer} that may process a supplied row.
	 * @return a positive value if there was a row fetched and the {@code rowConsumer} has been invokeked successfully.
	 * @return zero if a query is completed.
	 * @return a negative value if an error has occurred.
	 */
	int Next( const RowConsumer &rowConsumer ) {
		if( !stmt ) {
			return -1;
		}

		const int code = ::sqlite3_step( stmt );
		if( code == SQLITE_ROW ) {
			return rowConsumer( SQLiteRowReader( stmt ) ) ? +1 : -1;
		}
		if( code == SQLITE_DONE ) {
			return 0;
		}

		const char *format = S_COLOR_RED "SQLiteSelectAdapter::Next(): a statement step returned with `%s`\n";
		Com_Printf( format, ::sqlite3_errstr( code ) );
		(void)::sqlite3_finalize( stmt );
		stmt = nullptr;
		return -1;
	}

	/**
	 * Tries to read rows sequentially until a query is completed.
	 * Applies a {@code RowConsumer} for every read row.
	 * @param rowConsumer a {@code RowConsumer} that could process a row.
	 * @return a number of read rows on success, a negative value on failure
	 */
	int TryReadAll( const RowConsumer &rowConsumer ) {
		int numRows = 0;
		for(;; ) {
			int stepResult = Next( rowConsumer );
			if( stepResult > 0 ) {
				numRows++;
				continue;
			}
			// A negative value indicates a failure
			return stepResult == 0 ? numRows : -1;
		}
	}
};

LocalReportsStorage::LocalReportsStorage( const char *databasePath_ ) {
	// Actually never fails... or a failure is discovered immediately
	this->databasePath = ::strdup( databasePath_ );

	const char *tag = "LocalReportsStorage::LocalReportsStorage()";

	ScopedConnectionGuard connection( this );
	if( !connection ) {
		const char *format = "%s: Can't open an initial connection. Is the path `%s` valid and accessible?\n";
		Com_Error( ERR_FATAL, format, tag, databasePath_ );
	}

	for( const char *tableName : { "pending_reports", "failed_reports" } ) {
		if( !CreateTableIfNeeded( connection, tableName ) ) {
			// Close the connection before triggering a failure.
			// We do not know what kind of locks SQLite uses.
			connection.ForceClosing();
			Com_Error( ERR_FATAL, "%s: Can't create or check existence of %s table\n", tag, tableName );
		}
	}

	Com_Printf( "A local match reports storage has been successfully initialized at `%s`\n", databasePath_ );
}

DbConnection LocalReportsStorage::NewConnection() {
	DbConnection connection = nullptr;
	const char *const tag = "LocalReportsStorage::NewConnection()";
	const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	if( ::sqlite3_open_v2( databasePath, &connection, flags, nullptr ) == SQLITE_OK ) {
		Com_DPrintf( "%s: An SQLite connection %p has been successfully opened\n", tag, (const void *)connection );
		return connection;
	}

	const char *format = S_COLOR_RED "%s: Can't open an SQLite connection to `%s`: `%s`\n";
	Com_Printf( format, tag, databasePath, sqlite3_errmsg( connection ) );
	(void )::sqlite3_close( connection );
	return nullptr;
}

void LocalReportsStorage::DeleteConnection( DbConnection connection ) {
	const int code = ::sqlite3_close( connection );
	if( code == SQLITE_OK ) {
		return;
	}

	constexpr const char *tag = "LocalReportsStorage::DeleteConnection";

	if( code == SQLITE_BUSY ) {
		const char *format =
			"%s: An SQLiteConnection %p has some non-finalized "
			"child resources and is going to remain open. Aborting...\n";
		Com_Error( ERR_FATAL, format, tag, (const void *)connection );
	}

	Com_Error( ERR_FATAL, "%s: Unknown ::sqlite3_close() error for %p. Aborting...\n", tag, (const void *)connection );
}

bool LocalReportsStorage::CreateTableIfNeeded( DbConnection connection, const char *table ) {
	constexpr const char *format =
		"create table if not exists %s ("
		"	report_id text not null,"
		"	field_name text not null,"
		"	field_value text not null);";

	return SQLiteExecAdapter( connection ).ExecV( format, table );
}

bool LocalReportsStorage::WithinTransaction( std::function<bool( DbConnection )> &&block ) {
	ScopedConnectionGuard connection( this );
	if( !connection ) {
		return false;
	}

	SQLiteExecAdapter execAdapter( connection );
	if( !execAdapter.Begin() ) {
		return false;
	}

	if( block( connection ) ) {
		if( !execAdapter.Commit() ) {
			return false;
		}
	} else {
		if( !execAdapter.Rollback() ) {
			return false;
		}
	}

	return true;
}

bool LocalReportsStorage::Push( DbConnection connection, QueryObject *matchReport ) {
	// Sanity check...
	assert( matchReport->isPostQuery );
	assert( strstr( matchReport->url, "server/matchReport" ) );

	// We add a "synthetic" report_id field for partial reports.
	// They are purely for this reports storage system
	// to mark entries of different reports.
	// Even if they may be transmitted along with reports occasionally
	// they are meaningless for the Statsow server and get omitted.
	if( matchReport->FindFormParamByName( "report_id" ) ) {
		Com_Error( ERR_FATAL, "A `report_id` field is already present in the query object\n" );
	}

	// We may use an arbitrary random character sequence but let's use UUID's for consistency with the rest of the codebase.
	char reportIdAsString[UUID_BUFFER_SIZE];
	mm_uuid_t::Random().ToString( reportIdAsString );

	// We must set this to be able to recover report_id from query results
	matchReport->SetField( "report_id", reportIdAsString );

	const char *sql = "insert into pending_reports(report_id, field_name, field_value) values (?, ?, ?);";
	SQLiteInsertAdapter adapter( connection, sql );

	for( auto formParam = matchReport->formParamsHead; formParam; formParam = formParam->next ) {
		const string_view id( reportIdAsString, UUID_DATA_LENGTH );
		const string_view name( formParam->name, formParam->nameLen );
		const string_view value( formParam->value, formParam->valueLen );
		if( !adapter.InsertNextRow( id, name, value ) ) {
			return false;
		}
	}

	return true;
}

bool LocalReportsStorage::FetchNextReport( DbConnection connection, QueryObject *reportToFill ) {
	reportToFill->ClearFormData();

	// 1) Chose a random id in the CTE
	// 2) Select all rows that have ids matching the chosen id (ids are not really primary/potential keys).
	const char *sql =
		"with chosen as (select report_id as chosen_id from pending_reports order by random() limit 1) "
		"select field_name, field_value "
		"from pending_reports join chosen "
		"on pending_reports.report_id = chosen.chosen_id;";

	const auto rowConsumer = [&]( const SQLiteRowReader &reader ) -> bool {
		assert( reader.NumColumns() == 2 );
		const auto name( reader.GetString( 0 ) );
		const auto value( reader.GetString( 1 ) );
		reportToFill->SetField( name.data(), name.size(), value.data(), value.size() );
		return true;
	};

	SQLiteSelectAdapter adapter( connection, sql );
	if( adapter.TryReadAll( rowConsumer ) > 0 ) {
		reportToFill->hasConveredJsonToFormParam = true;
		return true;
	}

	return false;
}

const char *LocalReportsStorage::GetReportId( const QueryObject *matchReport ) {
	const char *reportIdAsString = matchReport->FindFormParamByName( "report_id" );
	if( !reportIdAsString ) {
		Com_Error( ERR_FATAL, "The match report object is missing `report_id` field\n" );
	}

	// Check whether it is a valid UUID for consistency
	// but do not actually convert result to UUID
	// (we would have to convert this back to string in that case)

	mm_uuid_t tmp;
	if( !Uuid_FromString( reportIdAsString, &tmp ) ) {
		Com_Error( ERR_FATAL, "The match report id string `%s` is not a valid UUID\n", reportIdAsString );
	}

	return reportIdAsString;
}

bool LocalReportsStorage::MarkReportAsSent( DbConnection connection, const QueryObject *matchReport ) {
	SQLiteExecAdapter adapter( connection );
	const char *reportId = GetReportId( matchReport );
	return adapter.ExecV( "delete from pending_reports where report_id = '%s'", reportId );
}

bool LocalReportsStorage::MarkReportAsFailed( DbConnection connection, const QueryObject *matchReport ) {
	// We could try using a CTE but that would look horrible.
	// Just execute 2 queries given we're in a transaction context.
	SQLiteExecAdapter adapter( connection );

	const char *reportId = GetReportId( matchReport );
	if( !adapter.ExecV( "insert into failed_reports select * from pending_reports where report_id = '%s'", reportId ) ) {
		return false;
	}

	return adapter.ExecV( "delete from pending_reports where report_id = '%s'", reportId );
}