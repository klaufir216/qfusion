#ifndef QFUSION_MM_STORAGE_H
#define QFUSION_MM_STORAGE_H

#include "mm_query.h"

#include <memory>
#include <utility>
#include <functional>
#include <random>

using DbConnection = struct sqlite3 *;

/**
 * Represents a reliable ACID storage for match reports.
 * This allows dumping many partial match reports produced by
 * several gametypes to the storage and send them sequentially later.
 * (A network call has to wait for a report confirmation from a remote host
 * and throughput of network pipeline is severely limited).
 */
class LocalReportsStorage {
	friend class ReliablePipe;
	friend struct ScopedConnectionGuard;

	char *databasePath { nullptr };

	explicit LocalReportsStorage( const char *databasePath_ );

	~LocalReportsStorage() {
		::free( databasePath );
	}

	/**
	 * An utility method that creates one of report tables ("pending_reports" or "failed_reports").
	 */
	bool CreateTableIfNeeded( DbConnection connection, const char *table );

	/**
	 * Creates a new database connection.
	 * Connections allows to perform operations on database and act as a transaction context.
	 * @return a non-zero (non-null) value if a new connection has been created successfully.
	 */
	DbConnection NewConnection();

	/**
	 * Deletes a database connection.
	 * All resources tied to it (like statements, bound parameters, etc.)
	 * must have been released to the moment of this call.
	 */
	void DeleteConnection( DbConnection connection );

	/**
	 * An utility method to get "report_id" field from a report.
	 * It must be already present for all reports kept in this storage.
	 */
	static const char *GetReportId( const QueryObject *matchReport );
public:
	/**
	 * Tries to store a report in a database.
	 * @param connection a connection that acts as a transaction context.
	 * @param query a match report object (that could be sent via network).
	 * @return true if the report has been successfully added to database.
	 * @note this method is assumed to be called within transaction.
	 * @note the report object lifecycle should be managed entirely by caller.
	 */
	bool Push( DbConnection connection, QueryObject *matchReport );

	/**
	 * Tries to fetch a random not-sent match report.
	 * @param connection a connection that acts as a transaction context.
	 * @param reportToFill a {@code QueryObject} to set form parameters loaded from the storage.
	 * @return true if some not sent yet report has been found and form parameters have been retrieved successfully.
	 * @note this method is assumed to be called within transaction.
	 */
	bool FetchNextReport( DbConnection connection, QueryObject *reportToFill );

	/**
	 * Marks a match report as sent (actually deletes it from pending reports).
	 * @param connection a connection that acts as a transaction context.
	 * @param matchReport a match report to mark as sent.
	 * @note this method is assumed to be called within transaction.
	 * Call it if the network service confirmed successful report delivery and processing of report at remote host.
	 */
	bool MarkReportAsSent( DbConnection connection, const QueryObject *matchReport );
	/**
	 * Marks a match report as failed (moves it from pending reports to sent reports table)
	 * @param connection a connection that acts as a transaction context.
	 * @param matchReport a match report to mark as failed.
	 * @note this method is assumed to be called within transaction.
	 * Do not call it on network failure or temporary remote host failure.
	 * Call this only if there is a certain schema or logic error tied to the report has been detected.
	 */
	bool MarkReportAsFailed( DbConnection connection, const QueryObject *matchReport );

	/**
	 * Executes a block of a code within transaction
	 * @param block a block of a code that should return true if a transaction should be committed.
	 * @return true if a transaction lifecycle has been completed successfully (begin/commit/rollback calls succeeded).
	 */
	bool WithinTransaction( std::function<bool( DbConnection )> &&block );
};

#endif
