defmodule Mix.Tasks.Compile.FlutterEmbedder do
  use Mix.Task.Compiler

  @recursive true

  def clean() do
    System.cmd("flutter", ["clean"], into: IO.stream(:stdio, :line))
  end

  def run(args) do
    _ = Mix.Project.get!()

    app_dir = Mix.Project.app_path()
    priv_dir = Path.join([app_dir, "priv"])

    flutter_assets = Path.join(priv_dir, "flutter_assets")
    unless File.dir?(flutter_assets), do: File.mkdir_p!(flutter_assets)

    manifest_file = Path.join(priv_dir, "flutter.manifest")
    manifest = read_manifest(manifest_file)
    target_files = Path.wildcard("lib/**/*.dart")
    target_manifest = create_manifest(target_files)

    cond do
      "--force" in args ->
        recompile(priv_dir)

      # naive check for created/deleted files.
      Enum.count(target_manifest) == Enum.count(manifest) ->
        maybe_recompile(target_manifest, manifest, priv_dir)

      true ->
        recompile(priv_dir)
    end

    write_manifest(target_manifest, manifest_file)

    {:ok, []}
  end

  def recompile(priv_dir) do
    Mix.shell().info("Compiling Flutter assets")
    flutter_assets = Path.join(priv_dir, "flutter_assets")
    {_, 0} = System.cmd("flutter", ["build", "bundle"], into: IO.stream(:stdio, :line))
    File.cp_r!("build/flutter_assets", flutter_assets)
  end

  def maybe_recompile(target_manifest, manifest, app_dir) do
    should_recompile? =
      Enum.reduce(0..Enum.count(target_manifest), false, fn
        _, true ->
          true

        index, false ->
          Enum.at(target_manifest, index) != Enum.at(manifest, index)
      end)

    if should_recompile?, do: recompile(app_dir)
  end

  def read_manifest(manifest_file) do
    unless File.exists?(manifest_file) do
      File.touch!(manifest_file)
    end

    File.read!(manifest_file)
    |> String.trim()
    |> String.split("\n")
    |> Enum.filter(&(String.length(&1) > 0))
    |> Enum.map(&String.split(&1, " "))
    |> Enum.map(fn [filename, hash] -> {filename, hash} end)
  end

  def write_manifest(target_manifest, manifest_file) do
    binary_manifest =
      Enum.map(target_manifest, fn {filename, hash} -> Enum.join([filename, hash], " ") end)
      |> Enum.join("\n")

    File.write!(manifest_file, binary_manifest)
  end

  # Example: [{"lib/main.dart", "FD7CC9C62AC0ED61B813F607D2368E49"}]
  def create_manifest(target_files) do
    Enum.map(target_files, fn filename ->
      {filename, File.read!(filename) |> :erlang.md5() |> Base.encode16()}
    end)
  end
end
