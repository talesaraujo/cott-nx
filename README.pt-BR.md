<h1 align=center>Crash of the Titans — Nintendo Switch port</h1>

<p align=center><a href="README.md">English</a> · <b>Português (Brasil)</b></p>

Port nativo e não oficial da versão de Xbox 360 de **Crash of the Titans** (2007) para o Nintendo Switch.

> [!WARNING]
> **Em desenvolvimento.** O jogo roda inteiro no Switch e é jogável, mas ainda há trabalho de desempenho e
> acabamento.

Não é um emulador. O código PowerPC do `default.xex` do jogo é recompilado estaticamente para C++ com
[ReXGlue](https://github.com/rexglue/rexglue-sdk) e compilado para o Switch, seguindo o caminho aberto pelo
[nfsmw-nx](https://github.com/StevensND/nfsmw-nx), o port de Need for Speed: Most Wanted.

> [!NOTE]
> Você precisa da sua própria cópia do jogo de Xbox 360. Nenhum arquivo do jogo, e nada gerado a partir dele, entra
> neste repositório.

## Edição suportada

| Arquivo | Tamanho | SHA-256 |
|---|---|---|
| `default.xex` (USA, En/Fr/De/Es/It/Nl) | 5.382.144 | `e76824b242c6c628738c373bdf052d456945d8e343eb934100617fd423e86373` |
| `default.rcf` | 1.266.263.595 | `42c04b348b02a47ed1fd98ce15b93907843de4d0977aa4d21f0722f1e9283874` |

## Onde estamos

1. **Fase 0, extrair o disco e identificar o executável:** concluída.
2. **Fase 1, rodar no PC (Linux) com ReXGlue:** concluída.
3. **Fase 2, levar para o Switch:** concluída.
4. **Fase 3, renderizador nativo em Vulkan e shaders traduzidos antes de jogar:** concluída.
5. **Fase 4, desempenho:** em andamento. No modo portátil, as fases rodam a ~29–31 FPS com resolução dinâmica
   (~0,75–0,9 do 720p); na base, em 720p, a ~28–30 FPS. Tudo nos clocks de fábrica.

## Destaques

### Feito para o Switch

- **Botões do Switch, no layout do Switch.** Os botões valem pela posição, como no controle original: o botão de
  baixo (o B da Nintendo) faz o que o A do Xbox fazia. As imagens dos botões do Xbox 360 nas dicas e menus do jogo
  viram as do Switch correspondentes (A, B, X, Y, +, −, L, R, ZL, ZR). `cott_button_icons = "xbox"` traz de volta as
  originais.
- **"Nintendo Switch" no lugar de "Xbox 360"** nos textos do jogo (salvar e carregar, reconectar o controle, o
  perfil), em todos os idiomas do jogo.
- **O idioma do console.** O jogo segue o idioma do Switch, com inglês para os que ele não tem, e ganhou uma tradução
  nova em português do Brasil. As letras que o português usa e as fontes do jogo não têm (ã, õ, Ê, Ô…) são desenhadas
  nas fontes a partir dos próprios acentos do jogo.
- **Co-op local** para dois jogadores com Joy-Con e Pro Controller, e saves funcionando.
- **Ícone no menu de homebrew** feito com o logo e a arte do próprio jogo, gerado a partir da sua cópia (nada dele é
  distribuído). O NRO também pode ser aberto por um forwarder na Home.

### Renderização nativa, clocks de fábrica

- **Sem emulação de GPU.** Um renderizador nativo em Vulkan (NVK no Tegra X1) lê os comandos de GPU do jogo e os
  desenha direto, com os shaders do jogo traduzidos antes de jogar. O Xbox 360 desenhava cada quadro em três faixas
  para caber na EDRAM; aqui a cena é desenhada uma vez só.
- **Resolução dinâmica**, como no Breath of the Wild: entre 0,6 e 1,0 do 720p, ajustada a cada meio segundo para a
  GPU caber no tempo do quadro. Ela acompanha a entrada e a saída da base, e as imagens novas são criadas numa thread
  à parte, então a troca não engasga.
- **Sem overclock.** No modo portátil, o port pede ao sistema o perfil oficial de GPU a 460,8 MHz, o mesmo que jogos
  comerciais usam.
- **Pontos quentes em código nativo, autoconferidos.** O decodificador de vídeo Bink e o skinning do jogo na CPU rodam
  em C++ nativo. Cada um roda lado a lado com o original recompilado nas primeiras chamadas e, de vez em quando,
  depois; qualquer diferença vai para o log e desliga a versão nativa.
- **Consertados no caminho:** os diálogos em XMA 5.1 e os vídeos Bink da abertura tocam corretamente.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `sdk/` | ReXGlue SDK v0.10.0 com a camada Horizon do nfsmw-nx e as correções deste port (procure por `COTT:`) |
| `tools/` | Extração do disco, download das bibliotecas de terceiros, ferramentas de análise do código gerado |
| `assets/` | O disco extraído (ignorado pelo git) |

## Compilar o tradutor (Linux)

```sh
python3 tools/fetch_thirdparty.py
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
cmake --build out/host --target rexglue
# o executável fica em sdk/out/linux-amd64/rexglue
```

No Fedora, os pacotes de desenvolvimento necessários:

```sh
sudo dnf install gtk3-devel glslc libshaderc-devel libXcursor-devel libXi-devel libXrandr-devel libXScrnSaver-devel \
  libXfixes-devel wayland-protocols-devel libdecor-devel mesa-libEGL-devel mesa-libGL-devel mesa-libgbm-devel \
  pipewire-devel pulseaudio-libs-devel alsa-lib-devel dbus-devel systemd-devel libusb1-devel
```

## Extrair o jogo

```sh
python3 tools/fase1_extraer.py "Crash of the Titans (USA).iso" -o assets/game_root
```

## Idioma

O idioma do jogo é escolhido pela opção `cott_language`, no `cott.toml` ao lado do `cott.nro`:

```toml
cott_language = "en"   # auto (padrão), en, fr, de, es, it ou pt-BR
```

- `auto` segue o idioma do Switch: inglês, espanhol, francês, alemão e italiano (com as vozes dubladas do jogo), ou
  pt-BR com o Switch em português. Qualquer outro idioma do Switch dá inglês.
- `pt-BR` é a tradução em português do Brasil feita pelo port: textos em português e vozes em inglês, a dublagem
  original. Nenhum idioma do jogo é substituído; com `en`, o jogo fica exatamente como era.

A tradução fica em [`translations/`](translations/README.md), que explica como ajudar a melhorá-la.

## Créditos

- **Radical Entertainment e Vivendi Games / Activision**, criadores de Crash of the Titans. Este é um port de fãs,
  sem nenhuma afiliação.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: nfsmw-nx, o modelo deste port (camada Horizon, ferramentas,
  documentação).
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: o ReXGlue SDK, construído sobre o trabalho da equipe do
  **[Xenia](https://xenia.jp)**.
- **[OAleex](https://github.com/OAleex/MojoRecomp)**: MojoRecomp, o port de PC de Crash of the Titans, cujo mapa do
  executável orienta este trabalho.
- **[Maxilure](https://github.com/Maxilure/MindOver-Recomp)**: Mind over Recomp, o port de PC de Crash: Mind over
  Mutant, com as correções do SDK e as descobertas sobre o motor da Radical.
- As bibliotecas listadas em [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

Sem afiliação com a Activision, a Radical Entertainment, a Microsoft ou a Nintendo. "Crash Bandicoot" e "Crash of the
Titans" são marcas de seus respectivos donos. Este repositório não contém nenhum asset nem código do jogo original.

Código-fonte sob a GPL-3.0 (veja [LICENSE](LICENSE)). As mudanças no `sdk/` ficam sob a licença BSD-3-Clause do SDK
(veja [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
