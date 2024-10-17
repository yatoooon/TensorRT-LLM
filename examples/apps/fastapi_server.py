import argparse
import asyncio
import json
from typing import AsyncGenerator, Optional

import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, Response, StreamingResponse

from tensorrt_llm.executor import GenerationExecutorWorker, GenerationResult

TIMEOUT_KEEP_ALIVE = 5  # seconds.
TIMEOUT_TO_PREVENT_DEADLOCK = 1  # seconds.
app = FastAPI()
executor: Optional[GenerationExecutorWorker] = None


@app.get("/stats")
async def stats() -> Response:
    assert executor is not None
    return JSONResponse(json.loads(await executor.aget_stats()))


@app.get("/health")
async def health() -> Response:
    """Health check."""
    return Response(status_code=200)


@app.post("/generate")
async def generate(request: Request) -> Response:
    assert executor is not None
    """Generate completion for the request.

    The request should be a JSON object with the following fields:
    - prompt: the prompt to use for the generation.
    - stream: whether to stream the results or not.
    - other fields: the sampling parameters (See `SamplingParams` for details).
    """
    request_dict = await request.json()

    prompt = request_dict.pop("prompt", "")
    streaming = request_dict.pop("streaming", False)
    promise = executor.generate_async(prompt, streaming, **request_dict)
    assert isinstance(promise, GenerationResult)

    async def stream_results() -> AsyncGenerator[bytes, None]:
        async for output in promise:
            yield (json.dumps(output.text_diff) + "\0").encode("utf-8")

    if streaming:
        return StreamingResponse(stream_results())

    # Non-streaming case
    await promise.aresult()
    return JSONResponse({"text": promise.text})


async def main(args):
    global executor

    with GenerationExecutorWorker(args.model_dir, args.tokenizer_type,
                                  args.max_beam_width) as executor:
        executor.block_subordinates()
        config = uvicorn.Config(app,
                                host=args.host,
                                port=args.port,
                                log_level="info",
                                timeout_keep_alive=TIMEOUT_KEEP_ALIVE)
        await uvicorn.Server(config).serve()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir")
    parser.add_argument("tokenizer_type")
    parser.add_argument("--host", type=str, default=None)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--max_beam_width", type=int, default=1)
    args = parser.parse_args()

    asyncio.run(main(args))
