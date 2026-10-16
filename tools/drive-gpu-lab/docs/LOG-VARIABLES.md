# GPU Lab — variáveis para coleta de logs

Referência para o repositório [`wonderkast02/all-tests-wonder`](https://github.com/wonderkast02/all-tests-wonder). Apenas **nome, valor, finalidade e ressalvas**, sem tutorial de configuração. Nem todas as variáveis se aplicam a todas as builds do Winlator/Bannerlator. **Não habilitar tudo de uma vez**: traces profundos podem alterar timings, produzir arquivos enormes e expor caminhos pessoais.

## DXVK

| Variável | Valor | Uso |
|---|---|---|
| `DXVK_LOG_LEVEL` | `info` | Informações, avisos e erros; modo padrão recomendado |
| `DXVK_LOG_LEVEL` | `debug` | Diagnóstico detalhado; mais sobrecarga |
| `DXVK_LOG_PATH` | `<PASTA_GRAVAVEL_DO_CONTAINER>` | Destino de `*_d3d9.log`, `*_d3d11.log` e outros logs, quando suportado |
| `DXVK_DEBUG` | `hang` | Diagnóstico adicional de hangs/device-lost, **apenas em versões que oferecem esse modo** |
| `DXVK_DEBUG` | `validation` | Modo de validação, requer suporte e camadas instaladas |
| `DXVK_PERF_EVENTS` | `1` | Encaminhamento de marcadores quando suportado; não cria trace PanVK sozinho |

## Wine

| Variável | Valor | Uso |
|---|---|---|
| `WINEDEBUG` | `+err,+warn,+seh` | Erros, avisos e exceções (volume menor) |
| `WINEDEBUG` | `+err,+warn,+seh,+vulkan,+d3d,+dxgi` | Diagnóstico de tradução gráfica, maior volume |
| `WINEDEBUG` | `+vulkan` | Rastreio pelo canal Wine; **não prova execução/retorno interno PanVK** |

## VKD3D-Proton (jogos D3D12)

| Variável | Valor | Uso |
|---|---|---|
| `VKD3D_DEBUG` | `warn` | Avisos e erros |
| `VKD3D_DEBUG` | `trace` | Diagnóstico profundo; log volumoso |
| `VKD3D_SHADER_DEBUG` | `warn` | Mensagens do compilador de shaders |
| `VKD3D_LOG_FILE` | `<ARQUIVO_GRAVAVEL>/vkd3d-proton.log` | Destino de log para a implementação compatível |

## Mesa/PanVK

| Variável | Valor | Uso |
|---|---|---|
| `MESA_LOG_FILE` | `<CAMINHO_NATIVO_GRAVAVEL>/mesa.log` | Arquivo de mensagens Mesa se recebido pela biblioteca nativa |
| `MESA_LOG_FILE_AUTO` | `1` | Arquivo por processo em `/tmp`; pode não ser encontrado pelo GPU Lab |
| `MESA_DEBUG` | `1` | Diagnóstico principalmente OpenGL/Gallium; **não é tracing interno PanVK** |

Não apresentar `PANVK_DEBUG` ou semelhantes como opções universais: a disponibilidade precisa ser comprovada na compilação Mesa/PanVK utilizada. `VK_ERROR_DEVICE_LOST`, submits e fences internos exigem log real/instrumentação, não apenas variáveis genéricas.

## Vulkan — condicionado à presença do loader/layers adequados

| Variável | Valor | Uso |
|---|---|---|
| `VK_LOADER_DEBUG` | `error,warn,driver` | **Somente Vulkan loader desktop**; a documentação Khronos informa que **não opera no loader Android** |
| `VK_INSTANCE_LAYERS` | `VK_LAYER_KHRONOS_validation` | Somente com validation layer instalada, acessível e compatível |
| `VK_LAYER_PATH` | `<DIRETORIO_DE_MANIFESTOS_VALIDOS>` | Busca da layer no loader desktop; **não** é pasta de logs |

## Box64/FEX — quando esse runtime é realmente utilizado

| Variável | Valor | Uso |
|---|---|---|
| `BOX64_LOG` | `1` | Logging normal de Box64 |
| `BOX64_LOG` | `2` | Logging detalhado |
| `BOX64_DYNAREC_LOG` | `1` | Logs mínimos do recompilador |
| `BOX64_DYNAREC_LOG` | `2` | Logs de depuração; sobrecarga |
| `FEX_SILENTLOG` | `0` | Permite logs FEX quando suportado pela build |
| `FEX_OUTPUTLOG` | `stderr` | Envia saída FEX para stderr, exige capturar esse fluxo |

## GPU Lab — variáveis de coleta no Termux

| Variável | Valor | Uso |
|---|---|---|
| `GPU_LAB_DIAG_MODE` | `deep` | Captura Android ampliada dentro das permissões existentes |
| `GPU_LAB_DRIVER_PATH` | `<CAMINHO_DA_SO_CANDIDATA>` | SHA256 da biblioteca indicada; **não prova qual biblioteca foi carregada pelo jogo** |
| `GPU_LAB_EXTRA_LOG_ROOT` | `<PASTA_EXTRA_DE_LOGS>` | Acrescenta raiz acessível para varredura de arquivos |
| `GPU_LAB_DOCUMENTS` | `/sdcard/Documents` | Raiz base de busca de logs externos |

## Regras de evidência

- Os ZIPs de jogo vão para `Download/GPU-Lab/Logs/<exe>-<data>-<PID>-<id>.zip` e o RETURN para `Download/Log Termux/`.
- `DXVK_LOG_LEVEL=debug` não garante logs PanVK; `VK_LOADER_DEBUG` no Android não garante saída; camadas não instaladas não são ativadas por variável.
- Verificar se o caminho de log é gravável e **incluído entre os diretórios realmente coletados**. Logs temporários fora dessas raízes podem desaparecer.
- Logs Wine/DXVK podem conter dados privados. Não publicar logs brutos da comunidade em GitHub por padrão.
- **Estado da R5.5 no teste de 2026-10-16:** ZIP e evidências DXVK/Wine íntegros; crash de interface `0xC0000005` e RETURN por `Ctrl+C`; build ainda não apta à release estável.

## Fontes oficiais / upstream

- [DXVK](https://github.com/doitsujin/dxvk/blob/master/README.md)
- [Wine — WINEDEBUG](https://www.winehq.org/pipermail/wine-cvs/2019-April/134591.html)
- [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton/blob/master/README.md)
- [Mesa](https://docs.mesa3d.org/envvars.html)
- [Khronos Vulkan Loader Debugging](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDebugging.md)
- [Box64](https://github.com/ptitSeb/box64/blob/main/docs/USAGE.md)
- [FEX configuration](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/Config/Config.json.in)
