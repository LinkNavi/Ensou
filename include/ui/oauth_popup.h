#pragma once

// ─── OAuthSetup ───────────────────────────────────────────────────────────────
// Animated multi-step guide that walks the user through creating an osu! OAuth
// app and fetching a client_credentials token.
//
// Call OAuthPopup::Show() to open it.
// Call OAuthPopup::UpdateDraw() every frame while IsOpen().

namespace OAuthPopup {

void Show();
void Hide();
bool IsOpen();
void UpdateDraw();

} // namespace OAuthPopup
