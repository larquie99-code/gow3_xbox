# God of War III — pacote de testes para Xbox

1. No GitHub, abra **Actions → Xbox build** e selecione a execução da branch
   `coderabbit/integrate-vulkan-memory-allocator/4638f294`.
2. Aguarde os jobs **Dozen for UWP** e **Xbox package** terminarem com sucesso.
3. Em **Artifacts**, baixe `GodOfWar3Launcher-<versão>` (requer login no GitHub).
   O resumo do job também fornece o link. Os artefatos ficam disponíveis por 30 dias.
4. Extraia o artefato e depois o ZIP do instalador. Ele contém o `.appx` e
   a pasta `Dependencies`. O arquivo `.sha256` permite conferir o ZIP usando
   `Get-FileHash -Algorithm SHA256 <arquivo.zip>` no PowerShell.
5. No Xbox Series X|S em **Developer Mode**, abra o Device Portal no PC,
   selecione **Add** e envie o `.appx` com as dependências. Defina o tipo como **Game**.
6. Use seu próprio dump de God of War III Remastered; arquivos do jogo não acompanham
   o pacote. Siga [as instruções completas](README.md) para preparar o jogo.

## Verificações no console

- Confira nome, ícones e imagem de abertura.
- Na tela inicial, alterne inglês, português brasileiro e espanhol; feche e reabra
  para conferir a persistência.
- Abra as configurações com **RS + LT**. Em **Image → Display → VSync**, alterne
  ligado/desligado e confira no log `CoreWindow VSync on/off applied (SyncInterval=1/0)`.
  O efeito na apresentação depende também da TV e do compositor do console.
- Experimente os quatro perfis de desempenho. As opções de cache de pipelines,
  gravação de comandos e memória de upload exigem fechar e reabrir o aplicativo.
- Compare travamentos, uso de memória e tempo de carregamento entre a primeira
  execução e as seguintes. Registre a versão, o perfil e as opções usadas.

Este pacote é experimental; a conclusão do build não comprova estabilidade ou
jogabilidade no console. Se o build falhar, consulte o log do job e, quando gerado,
`xbox-build-diagnostics`.

Para executar manualmente, o workflow precisa estar registrado na branch padrão.
Em **Run workflow**, selecione a branch desejada. A publicação de uma pré-release
é opcional e fica desativada por padrão.

## Texturas e cenas com muitos efeitos

Em **Performance**, comece com **Texture staging pool: Balanced**, **Preallocate
texture staging**, **Prewarm texture translation**, **Pipeline disk cache** e
**Batch image barriers** ligados. Deixe **Background pipeline warmup** desligado
para concluir a preparação dos pipelines salvos antes de jogar. Feche e reabra o app
após mudar essas opções.

Compare a mesma cena na primeira passagem, na segunda sem reiniciar e após fechar e
reabrir o app. Se houver memória disponível, compare o perfil **Streaming**; ele reserva
mais memória para evitar alocações durante rajadas. Em **Image**, a opção **Texture
sampling detail: +1** prefere mips menores na amostragem, reduzindo nitidez; ela não
reduz o tamanho residente das texturas. BC7 nativo é preservado. Conversão forçada
BC7/ASTC e remoção de mips residentes não estão implementadas.
