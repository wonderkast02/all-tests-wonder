package main

// optionalShortcut is only called from the GPU Lab foreground window.
// Left Control only: the app must never register system-wide combinations.
func optionalShortcut(key uint32, leftCtrl bool) string {
	if !leftCtrl {
		return ""
	}
	switch key {
	case 'A':
		return "panel"
	case 'B':
		return "marker"
	case 'C':
		return "deep"
	case 'D':
		return "screenshot"
	case 'E':
		return "stop"
	default:
		return ""
	}
}
