#include "telegram_i18n.h"
#include "format_utils.h" // formatUptime

// Brazilian Portuguese (pt-BR) throughout - "câmera"/"você". Say so if
// European Portuguese (pt-PT) is ever wanted instead; every function below
// would need its Portuguese branch revisited, not just a couple of words.

static String yesNo(bool lang_is_pt, bool value) {
  if (lang_is_pt) return value ? "sim" : "não";
  return value ? "yes" : "no";
}

String trMotionCaption(TelegramLang lang, const String& cameraName, const String& timestamp, bool isPetEvent) {
  if (lang == TelegramLang::Portuguese) {
    return isPetEvent ? "\xF0\x9F\x90\xBE " + cameraName + " - animal detectado - " + timestamp
                       : cameraName + " - " + timestamp;
  }
  return isPetEvent ? "\xF0\x9F\x90\xBE " + cameraName + " - pet detected - " + timestamp
                     : cameraName + " - " + timestamp;
}

String trPetAlertText(TelegramLang lang, const String& cameraName, const String& timestamp) {
  if (lang == TelegramLang::Portuguese) return "\xF0\x9F\x90\xBE " + cameraName + " - animal detectado - " + timestamp;
  return "\xF0\x9F\x90\xBE " + cameraName + " - pet detected - " + timestamp;
}

String trTimelapseCaption(TelegramLang lang, const String& cameraName, const String& timestamp) {
  if (lang == TelegramLang::Portuguese) return "\xF0\x9F\x95\x92 " + cameraName + " - captura agendada - " + timestamp;
  return "\xF0\x9F\x95\x92 " + cameraName + " - scheduled snapshot - " + timestamp;
}

String trTamperCaption(TelegramLang lang, const String& cameraName, const String& timestamp) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " - ADULTERA\xC3\x87\xC3\x83O DETECTADA - " + timestamp;
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " - TAMPER DETECTED - " + timestamp;
}

String trSignalLossMessage(TelegramLang lang, const String& cameraName, const String& timestamp) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " - PERDA DE SINAL DE V\xC3\x8D" "DEO - " + timestamp;
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " - VIDEO SIGNAL LOSS - " + timestamp;
}

String trMotionDigest(TelegramLang lang, const String& cameraName, uint32_t count, unsigned long elapsedSec) {
  if (lang == TelegramLang::Portuguese) {
    return cameraName + ": movimento continuou - mais " + String(count) + " evento(s) nos \xC3\xBAltimos " +
           String(elapsedSec) + " segundo(s).";
  }
  return cameraName + ": motion continued - " + String(count) + " more event(s) in the last " +
         String(elapsedSec) + " second(s).";
}

String trCameraOffline(TelegramLang lang, const String& cameraName, unsigned long minutes) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " est\xC3\xA1 OFFLINE - sem resposta h\xC3\xA1 mais de " +
           String(minutes) + " minuto(s).";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " is OFFLINE - no response for over " + String(minutes) +
         " minute(s).";
}

String trCameraBackOnline(TelegramLang lang, const String& cameraName) {
  if (lang == TelegramLang::Portuguese) return "\xE2\x9C\x85 " + cameraName + " est\xC3\xA1 ONLINE novamente.";
  return "\xE2\x9C\x85 " + cameraName + " is back ONLINE.";
}

String trSubscriptionLost(TelegramLang lang, const String& cameraName, unsigned long minutes) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " est\xC3\xA1 respondendo, mas n\xC3\xA3o mant\xC3\xA9m uma "
           "inscri\xC3\xA7\xC3\xA3o ONVIF v\xC3\xA1lida h\xC3\xA1 mais de " + String(minutes) +
           " minuto(s) - N\xC3\x83O est\xC3\xA1 recebendo eventos de movimento/adultera\xC3\xA7\xC3\xA3o. "
           "Verifique as credenciais, o modo WS-Security ou o suporte a eventos ONVIF da c\xC3\xA2mera.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + " is responding but hasn't held a working ONVIF "
         "subscription in over " + String(minutes) + " minute(s) - it is NOT receiving motion/tamper "
         "events. Check credentials, WS-Security mode, or the camera's ONVIF eventing support.";
}

String trMotionWatchdogTripped(TelegramLang lang, const String& cameraName, unsigned hours) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": nenhum movimento detectado h\xC3\xA1 mais de " +
           String(hours) + " hora(s) - verifique a c\xC3\xA2mera/sensor PIR.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": no motion detected in over " + String(hours) +
         " hour(s) - check the camera/PIR.";
}

String trNvsUsageWarning(TelegramLang lang, unsigned pct) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F O armazenamento NVS est\xC3\xA1 " + String(pct) + "% cheio - esta placa "
           "j\xC3\xA1 descartou grava\xC3\xA7\xC3\xB5""es silenciosamente aqui antes, quando encheu. Verifique a "
           "p\xC3\xA1gina Firmware e considere remover c\xC3\xA2meras/usu\xC3\xA1rios do Telegram n\xC3\xA3o "
           "utilizados.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F NVS storage is " + String(pct) + "% full - this board has silently "
         "dropped writes here before once it filled up. Check the Firmware page, and consider trimming "
         "unused cameras/Telegram users.";
}

String trWifiWeakWarning(TelegramLang lang, int rssi) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F O sinal WiFi est\xC3\xA1 fraco (" + String(rssi) + " dBm) - ainda "
           "conectado, mas considere reposicionar a placa/o roteador ou trocar de canal antes que piore o "
           "suficiente para cair de vez.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F WiFi signal is weak (" + String(rssi) + " dBm) - still connected, but "
         "consider the board/AP's placement or channel before it gets bad enough to actually drop.";
}

String trHeapLowWarning(TelegramLang lang, uint32_t baselineBytes, uint32_t maxAllocBytes) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F A mem\xC3\xB3ria heap (interna) livre atingiu um novo m\xC3\xADnimo de " +
           String(baselineBytes) + " bytes (maior bloco aloc\xC3\xA1vel: " + String(maxAllocBytes) +
           " bytes) - chegando perto de falhas de aloca\xC3\xA7\xC3\xA3o. Verifique o Log de Atividades "
           "neste hor\xC3\xA1rio para ver o que mais estava acontecendo (uma rajada de movimento, v\xC3\xA1rias "
           "c\xC3\xA2meras reconectando, ...).";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F Free (internal) heap hit a new low of " + String(baselineBytes) +
         " bytes (largest allocatable block: " + String(maxAllocBytes) + " bytes) - getting close to "
         "allocation-failure territory. Check the Activity log around this time for what else was "
         "happening (a motion burst, several cameras reconnecting, ...).";
}

String trCameraTaskSpawnFailure(TelegramLang lang, const String& cameraName) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": falha ao iniciar a tarefa de monitoramento "
           "(provavelmente falta de mem\xC3\xB3ria) - ela N\xC3\x83O est\xC3\xA1 sendo monitorada. Reiniciar a "
           "placa pode liberar mem\xC3\xB3ria suficiente para resolver isso.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": failed to start its monitoring task (likely out of "
         "memory) - it is NOT being monitored. A reboot may free enough memory to fix this.";
}

String trInternetOutageAlert(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F Queda de internet detectada - o roteador foi reiniciado (ciclo de "
           "energia).";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F Internet outage detected - power-cycled the router.";
}

String trBridgeOutageAlert(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F Queda da ponte das c\xC3\xA2meras detectada - o rel\xC3\xA9 da ponte foi "
           "acionado (ciclo de energia).";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F Camera bridge outage detected - power-cycled the bridge relay.";
}

String trSdFailure(TelegramLang lang, const String& reason) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F Falha no armazenamento do cart\xC3\xA3o SD (" + reason + ") e ele foi "
           "desativado pelo restante desta sess\xC3\xA3o - o hist\xC3\xB3rico de capturas voltou ao modo de "
           "reserva somente-PSRAM at\xC3\xA9 o pr\xC3\xB3ximo rein\xC3\xAD" "cio. Verifique o cart\xC3\xA3o/a "
           "fia\xC3\xA7\xC3\xA3o.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F SD card storage failed (" + reason + ") and has been disabled for the "
         "rest of this session - snapshot history is back to the PSRAM-only fallback until the next "
         "reboot. Check the card/wiring.";
}

String trSdCheckWarning(TelegramLang lang, size_t unreadableFiles, size_t filesChecked) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F A verifica\xC3\xA7\xC3\xA3o do armazenamento SD encontrou " +
           String((unsigned)unreadableFiles) + " arquivo(s) leg\xC3\xADvel(is) de " +
           String((unsigned)filesChecked) + " verificado(s). Veja a p\xC3\xA1gina Armazenamento do painel "
           "ou o log Serial para detalhes.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F SD storage check found " + String((unsigned)unreadableFiles) +
         " unreadable file(s) out of " + String((unsigned)filesChecked) + " checked. See the dashboard's "
         "Storage page or Serial log for details.";
}

String trMissingCredentials(TelegramLang lang, const String& cameraName, bool afterLiveEdit) {
  if (lang == TelegramLang::Portuguese) {
    return afterLiveEdit
        ? "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": nenhum usu\xC3\xA1rio/senha definido para esta "
          "c\xC3\xA2mera ap\xC3\xB3s a \xC3\xBAltima edi\xC3\xA7\xC3\xA3o - ela N\xC3\x83O est\xC3\xA1 sendo "
          "monitorada. Corrija pelo painel."
        : "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": nenhum usu\xC3\xA1rio/senha definido para esta "
          "c\xC3\xA2mera - ela N\xC3\x83O est\xC3\xA1 sendo monitorada.";
  }
  return afterLiveEdit
      ? "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": no username/password set for this camera after the "
        "last edit - it is NOT being monitored. Fix it via the dashboard."
      : "\xE2\x9A\xA0\xEF\xB8\x8F " + cameraName + ": no username/password set for this camera - it is NOT "
        "being monitored.";
}

String trTestMessage(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) {
    return "\xF0\x9F\xA7\xAA Mensagem de teste do painel Camera Monitor - se voc\xC3\xAA est\xC3\xA1 lendo "
           "isto, sua configura\xC3\xA7\xC3\xA3o do Telegram est\xC3\xA1 funcionando.";
  }
  return "\xF0\x9F\xA7\xAA Test message from the Camera Monitor dashboard - if you're reading this, your "
         "Telegram setup is working.";
}

String trHeartbeatHeader(TelegramLang lang, const String& firmwareVersion) {
  if (lang == TelegramLang::Portuguese) return "\xF0\x9F\x92\x93 Monitor de c\xC3\xA2meras v" + firmwareVersion + " - sinal de vida";
  return "\xF0\x9F\x92\x93 Camera monitor v" + firmwareVersion + " heartbeat";
}

String trUptimeLine(TelegramLang lang, unsigned long ms) {
  if (lang == TelegramLang::Portuguese) return "Tempo ativo: " + formatUptime(ms);
  return "Uptime: " + formatUptime(ms);
}

String trFreeHeapLine(TelegramLang lang, uint32_t freeBytes, uint32_t minEverBytes) {
  if (lang == TelegramLang::Portuguese) {
    return "Heap livre: " + String(freeBytes) + " bytes (m\xC3\xADnimo hist\xC3\xB3rico: " +
           String(minEverBytes) + ")";
  }
  return "Free heap: " + String(freeBytes) + " bytes (min ever: " + String(minEverBytes) + ")";
}

String trNvsUsageLine(TelegramLang lang, unsigned pct) {
  if (lang == TelegramLang::Portuguese) return "Uso do NVS: " + String(pct) + "%";
  return "NVS usage: " + String(pct) + "%";
}

String trWifiSignalLine(TelegramLang lang, int rssi) {
  if (lang == TelegramLang::Portuguese) return "Sinal WiFi: " + String(rssi) + " dBm";
  return "WiFi signal: " + String(rssi) + " dBm";
}

String trHeartbeatCameraLine(TelegramLang lang, const String& cameraName, bool subscribed, bool offline,
                              bool alertsEnabled, bool revertPending, bool revertToOn, const String& untilTime) {
  bool havUntil = untilTime.length() > 0;
  if (lang == TelegramLang::Portuguese) {
    String alertsNote;
    if (!alertsEnabled) {
      alertsNote = (revertPending && revertToOn && havUntil) ? " (alertas DESLIGADOS at\xC3\xA9 " + untilTime + ")"
                                                              : " (alertas DESLIGADOS)";
    } else if (revertPending && !revertToOn && havUntil) {
      alertsNote = " (alertas LIGADOS at\xC3\xA9 " + untilTime + ")";
    }
    return cameraName + ": " + (subscribed ? "inscrita" : "N\xC3\x83O inscrita") + (offline ? " (OFFLINE)" : "") +
           alertsNote;
  }
  String alertsNote;
  if (!alertsEnabled) {
    alertsNote = (revertPending && revertToOn && havUntil) ? " (alerts OFF until " + untilTime + ")"
                                                            : " (alerts OFF)";
  } else if (revertPending && !revertToOn && havUntil) {
    alertsNote = " (alerts ON until " + untilTime + ")";
  }
  return cameraName + ": " + (subscribed ? "subscribed" : "NOT subscribed") + (offline ? " (OFFLINE)" : "") +
         alertsNote;
}

String trBootHeader(TelegramLang lang, const String& firmwareVersion) {
  if (lang == TelegramLang::Portuguese) return "\xF0\x9F\x93\xB7 Monitor de c\xC3\xA2meras v" + firmwareVersion + " online";
  return "\xF0\x9F\x93\xB7 Camera monitor v" + firmwareVersion + " online";
}

String trRebootReasonLine(TelegramLang lang, const String& reasonText) {
  if (lang == TelegramLang::Portuguese) return "Motivo do rein\xC3\xAD" "cio: " + reasonText;
  return "Reboot reason: " + reasonText;
}

String trEnabledCamerasLine(TelegramLang lang, size_t enabledCount, size_t total) {
  if (lang == TelegramLang::Portuguese) {
    return String((unsigned)enabledCount) + "/" + String((unsigned)total) + " c\xC3\xA2meras habilitadas";
  }
  return String((unsigned)enabledCount) + "/" + String((unsigned)total) + " cameras enabled";
}

String trConfiguredCamerasHeader(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "--- C\xC3\xA2meras configuradas ---";
  return "--- Configured cameras ---";
}

String trSdBootCheckWarning(TelegramLang lang, size_t unreadableFiles, size_t directoriesChecked) {
  if (lang == TelegramLang::Portuguese) {
    return "\xE2\x9A\xA0\xEF\xB8\x8F A verifica\xC3\xA7\xC3\xA3o do SD na inicializa\xC3\xA7\xC3\xA3o encontrou " +
           String((unsigned)unreadableFiles) + " arquivo(s) leg\xC3\xADvel(is) em " +
           String((unsigned)directoriesChecked) + " diret\xC3\xB3rio(s) de c\xC3\xA2mera - veja a p\xC3\xA1gina "
           "Armazenamento do painel.";
  }
  return "\xE2\x9A\xA0\xEF\xB8\x8F SD boot check found " + String((unsigned)unreadableFiles) +
         " unreadable file(s) in " + String((unsigned)directoriesChecked) + " camera director(ies) - see "
         "the dashboard's Storage page.";
}

String trNotAuthorized(TelegramLang lang, const String& commandName) {
  if (lang == TelegramLang::Portuguese) return "Voc\xC3\xAA n\xC3\xA3o tem autoriza\xC3\xA7\xC3\xA3o para usar " + commandName + ".";
  return "You're not authorized to use " + commandName + ".";
}

String trRateLimited(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) {
    return "Voc\xC3\xAA est\xC3\xA1 enviando comandos r\xC3\xA1pido demais - aguarde um momento e tente "
           "novamente.";
  }
  return "You're sending commands too quickly - wait a moment and try again.";
}

String trStatusHeader(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Status de alertas das c\xC3\xA2meras:";
  return "Camera alert status:";
}

String trStatusCameraLine(TelegramLang lang, const String& cameraName, bool alertsEnabled, bool offline,
                            const String& timerSuffix, long avgLatencyMs) {
  String latencySuffix = avgLatencyMs >= 0 ? " ~" + String(avgLatencyMs) + "ms" : "";
  if (lang == TelegramLang::Portuguese) {
    return cameraName + ": " + (alertsEnabled ? "LIGADO" : "DESLIGADO") + (offline ? " - OFFLINE" : "") +
           timerSuffix + latencySuffix;
  }
  return cameraName + ": " + (alertsEnabled ? "ON" : "OFF") + (offline ? " - OFFLINE" : "") + timerSuffix +
         latencySuffix;
}

String trRebootingNow(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "\xE2\x99\xBB\xEF\xB8\x8F Reiniciando agora...";
  return "\xE2\x99\xBB\xEF\xB8\x8F Rebooting now...";
}

String trHelpText(TelegramLang lang, uint16_t eventLogCapacity, uint16_t maxDurationMinutes,
                    bool canCommand, bool canSnap, bool canReset) {
  bool pt = lang == TelegramLang::Portuguese;
  if (pt) {
    return
        "/status - lista o status de alerta de cada c\xC3\xA2mera\n"
        "/uptime - tempo ativo da placa\n"
        "/on <c\xC3\xA2mera|all> [dura\xC3\xA7\xC3\xA3o] - retoma os alertas\n"
        "/off <c\xC3\xA2mera|all> [dura\xC3\xA7\xC3\xA3o] - silencia os alertas\n"
        "/snap <c\xC3\xA2mera|all> - tira uma foto agora, ignorando sil\xC3\xAAncio/intervalo\n"
        "/on, /off ou /snap sem nome de c\xC3\xA2mera mostra bot\xC3\xB5""es de sele\xC3\xA7\xC3\xA3o (apenas "
        "ligar/desligar/foto permanentes, sem temporizador)\n"
        "/health - sa\xC3\xBA" "de da placa (heap, PSRAM, NVS, sinal WiFi, armazenamento SD)\n"
        "/log [N] - as N entradas mais recentes do log de atividades (padr\xC3\xA3o 10, m\xC3\xA1x " +
        String(eventLogCapacity) + ")\n"
        "/reset - reinicia a placa imediatamente\n"
        "/help - esta mensagem\n\n"
        "<c\xC3\xA2mera> corresponde por nome ou prefixo; \"all\" aplica a todas as c\xC3\xA2meras "
        "habilitadas.\n"
        "[dura\xC3\xA7\xC3\xA3o] \xC3\xA9 opcional: um n\xC3\xBAmero de minutos (m\xC3\xA1x " +
        String(maxDurationMinutes) + "), ou um hor\xC3\xA1rio no formato 24h como \"23:00\" (pr\xC3\xB3xima "
        "ocorr\xC3\xAAncia - amanh\xC3\xA3 se esse hor\xC3\xA1rio j\xC3\xA1 passou hoje). Se omitido, \xC3\xA9 "
        "permanente.\n\n"
        "Suas permiss\xC3\xB5""es: canCommand=" + yesNo(pt, canCommand) + ", canSnap=" + yesNo(pt, canSnap) +
        ", canReset=" + yesNo(pt, canReset);
  }
  return
      "/status - list every camera's alert status\n"
      "/uptime - board uptime\n"
      "/on <camera|all> [duration] - resume alerts\n"
      "/off <camera|all> [duration] - mute alerts\n"
      "/snap <camera|all> - fresh photo now, ignoring mute/cooldown\n"
      "/on, /off, or /snap with no camera name shows a tappable button picker instead (permanent "
      "on/off/snap only, no duration timer)\n"
      "/health - board health (heap, PSRAM, NVS, WiFi signal, SD storage)\n"
      "/log [N] - the N most recent Activity log entries (default 10, max " + String(eventLogCapacity) +
      ")\n"
      "/reset - reboot the board immediately\n"
      "/help - this message\n\n"
      "<camera> matches by name or prefix; \"all\" applies to every enabled camera.\n"
      "[duration] is optional: a number of minutes (max " + String(maxDurationMinutes) + "), or a 24h "
      "clock time like \"23:00\" (next occurrence - tomorrow if that time already passed today). Omitted "
      "means permanent.\n\n"
      "Your permissions: canCommand=" + yesNo(pt, canCommand) + ", canSnap=" + yesNo(pt, canSnap) +
      ", canReset=" + yesNo(pt, canReset);
}

String trHealthHeader(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Sa\xC3\xBA" "de da placa:";
  return "Board health:";
}

String trFreePsramLine(TelegramLang lang, uint32_t freeBytes) {
  if (lang == TelegramLang::Portuguese) return "PSRAM livre: " + String(freeBytes) + " bytes";
  return "Free PSRAM: " + String(freeBytes) + " bytes";
}

String trSdStorageLine(TelegramLang lang, const String& sdDetailText) {
  if (lang == TelegramLang::Portuguese) return "Armazenamento SD: " + sdDetailText;
  return "SD storage: " + sdDetailText;
}

String trSdDisabledDetail(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "desativado (hist\xC3\xB3rico de capturas somente em PSRAM)";
  return "disabled (PSRAM-only snapshot history)";
}

String trSdNotDetectedDetail(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "habilitado mas n\xC3\xA3o detectado (reserva somente-PSRAM ativa)";
  return "enabled but not detected (PSRAM-only fallback active)";
}

String trSdDetail(TelegramLang lang, const String& cardTypeName, double usedMB, double totalMB) {
  if (lang == TelegramLang::Portuguese) {
    return cardTypeName + ", " + String(usedMB, 1) + " MB / " + String(totalMB, 1) + " MB usados";
  }
  return cardTypeName + ", " + String(usedMB, 1) + " MB / " + String(totalMB, 1) + " MB used";
}

String trLogHeader(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Atividade recente:";
  return "Recent activity:";
}

String trLogEmpty(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Nada registrado ainda.";
  return "Nothing logged yet.";
}

String trElapsedSince(TelegramLang lang, unsigned long eventMs, unsigned long nowMs) {
  unsigned long elapsed = nowMs - eventMs;
  if (lang == TelegramLang::Portuguese) {
    if (elapsed < 60000UL) return "agora mesmo";
    return formatUptime(elapsed) + " atr\xC3\xA1s";
  }
  if (elapsed < 60000UL) return "just now";
  return formatUptime(elapsed) + " ago";
}

String trAmbiguousCamera(TelegramLang lang, const String& name, const String& matchList) {
  if (lang == TelegramLang::Portuguese) {
    return "\"" + name + "\" corresponde a mais de uma c\xC3\xA2mera: " + matchList + " - seja mais "
           "espec\xC3\xAD" "fico.";
  }
  return "\"" + name + "\" matches more than one camera: " + matchList + " - be more specific.";
}

String trUnknownCamera(TelegramLang lang, const String& name) {
  if (lang == TelegramLang::Portuguese) return "C\xC3\xA2mera desconhecida ou desativada: " + name;
  return "Unknown or disabled camera: " + name;
}

String trNoCamerasToChoose(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Nenhuma c\xC3\xA2mera habilitada para escolher.";
  return "No enabled cameras to choose from.";
}

String trCameraPickerPrompt(TelegramLang lang, const String& commandDisplayName) {
  if (lang == TelegramLang::Portuguese) return "Escolha uma c\xC3\xA2mera para " + commandDisplayName + ":";
  return "Choose a camera for " + commandDisplayName + ":";
}

String trCallbackDataTooLong(TelegramLang lang, size_t skipped, const String& commandDisplayName) {
  if (lang == TelegramLang::Portuguese) {
    return String((unsigned)skipped) + " nome(s) de c\xC3\xA2mera eram longos demais para exibir como bot\xC3\xA3o "
           "e foram omitidos da lista acima - use o comando de texto em vez disso (ex.: \"" +
           commandDisplayName + " <nome>\").";
  }
  return String((unsigned)skipped) + " camera name(s) were too long to show as a button and were left off "
         "the list above - use the text command instead (e.g. \"" + commandDisplayName + " <name>\").";
}

String trCallbackUnrecognized(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "A\xC3\xA7\xC3\xA3o n\xC3\xA3o reconhecida.";
  return "Unrecognized action.";
}

String trCallbackNotAuthorized(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "N\xC3\xA3o autorizado.";
  return "Not authorized.";
}

String trCallbackCameraGone(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Essa c\xC3\xA2mera n\xC3\xA3o est\xC3\xA1 mais dispon\xC3\xADvel.";
  return "That camera is no longer available.";
}

String trCameraNoLongerAvailable(TelegramLang lang, const String& target) {
  if (lang == TelegramLang::Portuguese) {
    return "\"" + target + "\" n\xC3\xA3o est\xC3\xA1 mais dispon\xC3\xADvel - ela pode ter sido renomeada, "
           "exclu\xC3\xAD" "da ou desativada desde que este bot\xC3\xA3o foi enviado.";
  }
  return "\"" + target + "\" is no longer available - it may have been renamed, deleted, or disabled since "
         "this button was sent.";
}

String trNoSnapshotUriYet(TelegramLang lang, const String& cameraName) {
  if (lang == TelegramLang::Portuguese) return cameraName + ": nenhuma URL de captura dispon\xC3\xADvel ainda.";
  return cameraName + ": no snapshot URI available yet.";
}

String trSnapshotFetchFailed(TelegramLang lang, const String& cameraName) {
  if (lang == TelegramLang::Portuguese) return cameraName + ": falha ao obter a captura - veja o log Serial.";
  return cameraName + ": snapshot fetch failed - see Serial log.";
}

String trDurationParseError(TelegramLang lang, const String& durationText, uint16_t maxMinutes) {
  if (lang == TelegramLang::Portuguese) {
    return "N\xC3\xA3o entendi a dura\xC3\xA7\xC3\xA3o \"" + durationText + "\" - use um n\xC3\xBAmero de "
           "minutos (ex.: \"30\", m\xC3\xA1x " + String(maxMinutes) + ") ou um hor\xC3\xA1rio no formato 24h "
           "(ex.: \"23:00\").";
  }
  return "Couldn't understand duration \"" + durationText + "\" - use a number of minutes (e.g. \"30\", max " +
         String(maxMinutes) + ") or a 24h clock time (e.g. \"23:00\").";
}

String trTimerSuffix(TelegramLang lang, bool turnOn, unsigned long durationMs) {
  if (lang == TelegramLang::Portuguese) {
    return " (auto-" + String(turnOn ? "DESLIGA" : "LIGA") + " em " + formatUptime(durationMs) + ")";
  }
  return " (auto " + String(turnOn ? "OFF" : "ON") + " in " + formatUptime(durationMs) + ")";
}

String trAlertsState(TelegramLang lang, const String& subject, bool turnOn, const String& suffix) {
  if (lang == TelegramLang::Portuguese) {
    return subject + " - alertas: " + String(turnOn ? "LIGADOS" : "DESLIGADOS") + suffix;
  }
  return subject + " alerts: " + String(turnOn ? "ON" : "OFF") + suffix;
}

String trTimerExpiredSuffix(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return " (tempo esgotado)";
  return " (timer expired)";
}

String trAllCamerasSubject(TelegramLang lang, size_t count) {
  if (lang == TelegramLang::Portuguese) return "Todas as " + String((unsigned)count) + " c\xC3\xA2mera(s)";
  return "All " + String((unsigned)count) + " camera(s)";
}

String trNoEnabledCameras(TelegramLang lang) {
  if (lang == TelegramLang::Portuguese) return "Nenhuma c\xC3\xA2mera habilitada para aplicar isso.";
  return "No enabled cameras to apply this to.";
}
