defmodule FlutterEmbedder.MethodCall.Handler do
  @type std_ok :: {:ok, StandardMessageCodec.t()}
  @type std_error_code :: StandardMessageCodec.dart_string()
  @type std_error_message :: StandardMessageCodec.dart_string()
  @type std_err :: {:error, std_error_code, std_error_message, StandardMessageCodec.t()}
  @type std_not_implemented :: :not_implemented
  @type channel :: PlatformChannelMessage.channel()
  @type method :: StandardCall.method()
  @type args :: StandardCall.args()

  @doc """
  https://flutter.dev/docs/development/platform-integration/platform-channels
  """
  @callback handle_std_call(channel, method, args) :: std_ok | std_err | std_not_implemented
end
