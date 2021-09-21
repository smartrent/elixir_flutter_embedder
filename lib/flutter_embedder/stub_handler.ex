defmodule FlutterEmbedder.StubMethodCallHandler do
  require Logger

  def handle_std_call(channel, method, args) do
    Logger.error("Unhandled std method call #{channel}:#{method}(#{inspect(args)})")
    :not_implemented
  end
end
