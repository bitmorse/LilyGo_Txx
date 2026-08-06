#ifndef FILEID_H
#define FILEID_H

// Parse the trailing file id from a ".../file/<id>" request URI. Returns the id (>= 0),
// or -1 if the last path segment is empty or not all decimal digits. This exists because
// the file server routes on the wildcard "/file/*": a bare atoi() maps "" and "abc" to 0
// (a valid id), so "DELETE /file/garbage" would destroy file 0. Rejecting non-numeric
// segments makes those 404 instead. Pure + host-tested (see test/test_fileid.c).
int fileid_from_uri(const char *uri);

#endif // FILEID_H
