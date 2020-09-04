defmodule FlutterEmbedder.StandardMethodCall do
  alias FlutterEmbedder.{StandardMessageCodec, PlatformChannelMessage}
  defstruct [:method, :args]

  @type method :: StandardMessageCodec.dart_string()
  @type args :: StandardMessageCodec.t()
  @type t :: %__MODULE__{
          method: method(),
          args: args()
        }

  @spec decode(PlatformChannelMessage.t()) :: {:ok, t()} | {:error, String.t()}
  def decode(%PlatformChannelMessage{message: message}) do
    with {method, args_} when is_binary(method) and byte_size(args_) > 0 <-
           StandardMessageCodec.decode_value(message),
         {args, ""} <- StandardMessageCodec.decode_value(args_) do
      {:ok,
       %__MODULE__{
         method: method,
         args: args
       }}
    else
      {value, _rest} -> {:error, "Method must be a string. got: #{inspect(value)}"}
    end
  rescue
    e in [StandardMessageCodec.DecodeError] ->
      {:error, e.message}

    exception ->
      reraise(exception, __STACKTRACE__)
  end
end
