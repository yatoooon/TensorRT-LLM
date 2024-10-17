import os.path
from argparse import ArgumentParser
from dataclasses import dataclass, fields
from subprocess import run
from sys import stderr, stdout
from typing import List, Literal, Union

split = os.path.split
join = os.path.join
dirname = os.path.dirname


@dataclass
class Arguments:
    download: bool = False
    dtype: Literal['float16', 'float32', 'bfloat16'] = 'float16'

    hf_repo_name: Literal['facebook/bart-large-cnn',
                          't5-small'] = 'facebook/bart-large-cnn'

    model_cache: str = '/llm-models'

    # override by --only_multi_gpu, enforced by test_cpp.py
    tp: int = 2
    pp: int = 2

    beams: int = 1
    gpus_per_node: int = 4
    debug: bool = False

    rm_pad: bool = True
    gemm: bool = True
    # rmsm: bool = True # TODO: remove this

    max_new_tokens: int = 10

    @property
    def ckpt(self):
        return self.hf_repo_name.split('/')[-1]

    @property
    def base_dir(self):
        return dirname(dirname(__file__))

    @property
    def data_dir(self):
        return join(self.base_dir, 'data/enc_dec')

    @property
    def models_dir(self):
        return join(self.base_dir, 'models/enc_dec')

    @property
    def hf_models_dir(self):
        return join(self.model_cache, self.ckpt)

    @property
    def trt_models_dir(self):
        return join(self.models_dir, 'trt_models', self.ckpt)

    @property
    def engines_dir(self):
        return join(self.models_dir, 'trt_engines', self.ckpt,
                    f'{self.tp * self.pp}-gpu', self.dtype)

    @property
    def model_type(self):
        return self.ckpt.split('-')[0]

    def __post_init__(self):
        parser = ArgumentParser()
        for k in fields(self):
            k = k.name
            v = getattr(self, k)
            if isinstance(v, bool):
                parser.add_argument(f'--{k}', default=int(v), type=int)
            else:
                parser.add_argument(f'--{k}', default=v, type=type(v))

        parser.add_argument('--only_multi_gpu', action='store_true')
        args = parser.parse_args()
        for k, v in args._get_kwargs():
            setattr(self, k, v)
        if args.only_multi_gpu:
            self.tp = 2
            self.pp = 2
        else:
            self.tp = 1
            self.pp = 1


@dataclass
class RunCMDMixin:
    args: Arguments

    def command(self) -> Union[str, List[str]]:
        raise NotImplementedError

    def run(self):
        cmd = self.command()
        if cmd:
            cmd = ' '.join(cmd) if isinstance(cmd, list) else cmd
            print('+ ' + cmd)
            run(cmd, shell='bash', stdout=stdout, stderr=stderr, check=True)


class DownloadHF(RunCMDMixin):

    def command(self):
        args = self.args
        return [
            'git', 'clone', f'https://huggingface.co/{args.hf_repo_name}',
            args.hf_models_dir
        ] if args.download else ''


class Convert(RunCMDMixin):

    def command(self):
        args = self.args
        return [
            f'python examples/enc_dec/convert_checkpoint.py',
            f'--model_type {args.model_type}',
            f'--model_dir {args.hf_models_dir}',
            f'--output_dir {args.trt_models_dir}',
            f'--tp_size {args.tp} --pp_size {args.pp}'
        ]


class Build(RunCMDMixin):

    def command(self):
        args = self.args
        engine_dir = join(args.engines_dir, f'tp{args.tp}')
        weight_dir = join(args.trt_models_dir, f'tp{args.tp}', f'pp{args.pp}')
        encoder_build = [
            f"trtllm-build --checkpoint_dir {join(weight_dir, 'encoder')}",
            f"--output_dir {join(engine_dir, 'encoder')}",
            f'--paged_kv_cache disable', f'--moe_plugin disable',
            f'--enable_xqa disable', f'--max_beam_width {args.beams}',
            f'--max_batch_size 8', f'--max_output_len 200',
            f'--gemm_plugin {args.dtype}',
            f'--bert_attention_plugin {args.dtype}',
            f'--gpt_attention_plugin {args.dtype}',
            f'--remove_input_padding enable', f'--context_fmha disable',
            '--use_custom_all_reduce disable'
        ]

        decoder_build = [
            f"trtllm-build --checkpoint_dir {join(weight_dir, 'decoder')}",
            f"--output_dir {join(engine_dir, 'decoder')}",
            f'--paged_kv_cache disable', f'--moe_plugin disable',
            f'--enable_xqa disable', f'--max_beam_width {args.beams}',
            f'--max_batch_size 8', f'--max_output_len 200',
            f'--gemm_plugin {args.dtype}',
            f'--bert_attention_plugin {args.dtype}',
            f'--gpt_attention_plugin {args.dtype}',
            f'--remove_input_padding enable', f'--context_fmha disable',
            '--max_input_len 1', '--use_custom_all_reduce disable'
        ]

        encoder_build = ' '.join(encoder_build)
        decoder_build = ' '.join(decoder_build)
        ret = ' && '.join((encoder_build, decoder_build))
        return ret


if __name__ == "__main__":
    # TODO: add support for more models / setup
    args = Arguments()
    DownloadHF(args).run()
    Convert(args).run()
    Build(args).run()
