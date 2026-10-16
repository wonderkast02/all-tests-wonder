package main

// Visual-only state machine. Never affects capture, results or archive validity.
func renderCapturePhase(running, done bool, pid, stage string) string {
	if done {
		return "complete"
	}
	if running || stage == "GRAVANDO" {
		return "recording"
	}
	if pid != "" && pid != "N/D" && pid != "0" {
		return "preparing"
	}
	return "waiting"
}
func visualPhaseCode(phase string) uint32 {
	switch phase {
	case "preparing":
		return 1
	case "recording":
		return 2
	case "complete":
		return 3
	}
	return 0
}

var phasePreparing = map[string]string{
	"pt": "INICIANDO", "en": "PREPARING", "es": "PREPARANDO", "fr": "PRÉPARATION", "de": "VORBEREITUNG", "it": "PREPARAZIONE", "ru": "ПОДГОТОВКА", "ar": "جارٍ التحضير", "he": "בהכנה", "zh": "准备中", "ja": "準備中", "ko": "준비 중", "tr": "HAZIRLANIYOR", "uk": "ПІДГОТОВКА", "pl": "PRZYGOTOWANIE", "fa": "در حال آماده‌سازی", "id": "MENYIAPKAN", "hi": "तैयारी",
}

func visualPhaseText(language, phase string) string {
	if phase == "preparing" {
		if v := phasePreparing[language]; v != "" {
			return v
		}
		return phasePreparing["en"]
	}
	return localize(language, phase)
}
