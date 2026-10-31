
#ifndef AUTH_H
#define AUTH_H

/* Loads saved tokens and refreshes them; if that fails, runs browser login.
   Returns 0 on success. */
int auth_init(void);

/* Refreshes the access token using the saved refresh token. */
int auth_refresh(void);

/* Returns the current access token. */
const char *auth_access_token(void);

#endif
