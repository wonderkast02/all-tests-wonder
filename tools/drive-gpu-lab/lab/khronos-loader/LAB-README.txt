Drive GPU Lab — Khronos Windows Vulkan Loader Lab
=================================================

Objetivo:
Validar, dentro do Wine/Winlator, a arquitetura:

G720VkLoaderCheck.exe
  -> Khronos vulkan-1.dll (Windows)
  -> G720VkLayer.dll
  -> winevulkan.dll como ICD
  -> Vulkan do host Android / PanVK

Este LAB é isolado. Ele não substitui o vulkan-1.dll do jogo e não modifica
a instalação do Wine. O checker carrega explicitamente o vulkan-1.dll que está
na mesma pasta, configura VK_LAYER_PATH/VK_DRIVER_FILES apenas no próprio
processo e grava loader-check-result.txt.

Uso:
1. Mantenha o jogo fechado.
2. Abra G720VkLoaderCheck.exe pelo Wine File Manager.
3. Aguarde a caixa PASS/FAIL.
4. Leia loader-check-result.txt nesta mesma pasta.
5. Não copie vulkan-1.dll para a pasta do jogo até o LAB reportar PASS.

PASS exige:
- Khronos loader carregado pelo caminho absoluto do LAB;
- VK_LAYER_DRIVE_G720_telemetry enumerada;
- VkInstance criada com a layer;
- winevulkan.dll carregada como ICD;
- pelo menos um VkPhysicalDevice enumerado;
- VkDevice criado com sucesso.
