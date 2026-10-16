# Changelog — GPU Lab

## R5.7 — PanVK Full Scan / GUI Safety (pré-versão experimental)

- Varredura PanVK por streaming com limite explícito (até 256 MiB por fonte e 512 MiB por sessão), contabilizando bytes não analisados e motivo de parada.
- Reconhecimento de mensagens `SYNC-A3`; diferencia texto de wait-path de confirmação de fence/submit.
- Sinalização de origem `SHARED_WINE_NOT_SESSION_ISOLATED` para logs Wine compartilhados.
- Recuperação de GUI com modo reduzido após queda, preservando monitor independente.
- `gui-last-exit.json` e opção `GPU_LAB_GUI_TRACE_SEH` para pesquisa de exceções Wine.
- `PANVK-EVIDENCE.json` e `DRIVER-EVIDENCE.json` dentro do ZIP do jogo; fontes ausentes não são inferidas.

## R5.6 — Pós-jogo e PanVK

- Finalização de segurança quando sessão encerrada e ZIP publicado; evita espera sem fim no Termux.
- Primeira versão do relatório PanVK/Mesa; perfis opcionais de ambiente sem injeção no jogo.

## R5.5 — Evidências Vulkan

- Classificação de menções a acquire, submit, present, waits, VUIDs, erros e mensagens do driver em logs disponíveis.
- Identidade SHA256 opcional de biblioteca **candidata**, sem inferir biblioteca carregada.

## R5.4 — Ciclo de vida

- Encerramento automático após saída confirmada do jogo; proteção contra duplicação por rodada.
- Coleta independente do estado da GUI; RETURN parcial explícito quando necessário.

## R5.3 — Detecção e interface

- Tentativa conservadora de anexação a jogos já em execução.
- Diagnóstico e leitura de informações de dispositivo; atalhos globais desativados após crash no Wine.

## R5.2 — Android

- Captura condicionada a permissões de logcat/kernel/sysfs; cobertura e indisponibilidades registradas.
- Modo de diagnóstico profundo opcional.

## R5.1 / R5 — Base estável

- Supervisor Windows e coletor Android isolados da GUI, identificação automática de jogo/EXE, telemetria de processo.
- Sessões únicas, ZIPs por jogo em `GPU-Lab/Logs` e RETURN separado em `Log Termux`.
- Manifesto por sessão, SHA256/CRC, integridade, deduplicação e arquivos sem sobrescrita.

**Limitação ainda aberta na R5.7:** a GUI pode sofrer exceção `0xC0000005` no Wine. A recuperação não comprova que a causa tenha sido corrigida.
