#pragma once
#include <string>

namespace muisc {

// OAuth 2.0 credentials for the Spotify Web API, persisted to
// $HOME/.config/mousiki/spotify_token.json (mode 0600).
struct SpotifyTokens {
    std::string access_token;
    std::string refresh_token;
    long long expires_at_unix = 0; // absolute expiry, not the relative
                                   // expires_in the server sends, so a
                                   // restart can tell a stale token from
                                   // a fresh one without re-querying.

    bool valid() const { return !access_token.empty(); }
    bool expired() const;
};

// Spotify Web API authorization, via the Authorization Code flow with
// PKCE (RFC 7636).
//
// PKCE needs no client secret, which means no hosted auth broker: the
// user registers their own free Spotify app, puts the (non-secret)
// client ID in config.txt, and the code->token exchange happens here.
// Nothing but Spotify ever sees the tokens.
class SpotifyAuth {
public:
    // Redirect URI registered in the user's Spotify app settings.
    static constexpr int kCallbackPort = 8350;
    static std::string redirect_uri();

    // Loads persisted tokens, if any.
    static SpotifyTokens load();

    // Persists tokens (creates the config dir, chmod 0600).
    static bool save(const SpotifyTokens& t);

    // Forgets the stored tokens (the "sign out" path).
    static bool clear();

    // Full interactive login. Builds the PKCE challenge, prints the
    // authorize URL (and tries to open a browser), then blocks on a
    // one-shot loopback HTTP server waiting for Spotify to redirect back
    // with ?code=. Exchanges the code for tokens and persists them.
    //
    // Blocks, so the caller runs it off the UI thread. Gives up after
    // `timeout_sec` so an abandoned browser tab doesn't leak the
    // listening socket.
    static bool interactive_login(const std::string& client_id,
                                  SpotifyTokens& out,
                                  std::string* error_out = nullptr,
                                  int timeout_sec = 300);

    // Exchanges the stored refresh_token for a fresh access_token.
    // Spotify may or may not return a new refresh_token; when it doesn't,
    // the existing one is preserved.
    static bool refresh(const std::string& client_id,
                        SpotifyTokens& tokens,
                        std::string* error_out = nullptr);

private:
    static std::string token_path();
};

} // namespace muisc
