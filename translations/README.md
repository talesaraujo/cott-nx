# Traduções

`pt_BR.txt` é a tradução do jogo para o português do Brasil, feita pelo port. O jogo a carrega com
`cott_language = "pt-BR"` (ou `auto` com o Switch em português). Ela vai dentro do executável: depois de mudar o
arquivo, compile de novo.

O arquivo **não contém o texto original do jogo**: cada linha é `<chave>\t<tradução>`, e a chave é um código de 64
bits calculado a partir do texto em inglês. É assim que o jogo acha a tradução de cada frase, em qualquer uma das
várias cópias dos pacotes de texto.

## Como melhorar a tradução

Você precisa do seu próprio `default.rcf` (veja "Extrair o jogo" no README principal).

```sh
python3 tools/text/export_po.py     # cria translations/work/pt_BR.po: o inglês + a tradução atual
# edite translations/work/pt_BR.po no Poedit, Lokalize, Weblate ou num editor de texto
python3 tools/text/compile_po.py    # grava translations/pt_BR.txt a partir do .po
```

- O `.po` tem o texto em inglês do jogo, por isso a pasta `translations/work/` fica fora do git. Envie só o
  `pt_BR.txt`.
- Cada frase traz onde aparece (`banco:hash`) e, como contexto, o texto em espanhol.
- `{A}`, `{B}`, `{X}`, `{Y}`, `{START}`, `{BACK}`, `{LT}`, `{RT}`, `{LB}`, `{RB}`, `{LSTICK}`, `{LEFT}`, `{RIGHT}` e
  `{ROTATE}` são os desenhos dos botões: mantenha os mesmos da frase original. O `compile_po.py` avisa quando faltam.
- Quebras de linha (`\n`) seguem o inglês: as caixas de texto do jogo têm tamanho fixo, então frases bem mais longas
  podem não caber.
- Frases sem tradução aparecem em inglês.
- As fontes do jogo não tinham ã, õ, Ã, Õ, Ê e Ô: o port as desenha a partir das letras que existem
  (`app/src/cott_language.cpp`). Pode usá-las normalmente.
