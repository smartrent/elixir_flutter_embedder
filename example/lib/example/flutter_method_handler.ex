defmodule Example.FlutterMethodHandler do
  @behaviour FlutterEmbedder.StandardMethodCall.Handler

  def handle_std_call("nerves/blue_heron", "setBulbColor", color) do
    GoveeBulb.set_color(GoveeBulb, color)
    {:ok, 100}
  end

  def handle_std_call(_channel, _method, _value) do
    :not_implemented
  end
end
