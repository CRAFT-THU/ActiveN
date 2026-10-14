"""Retained inputs must resolve after relocation without recreating old paths."""
from pathlib import Path
import tempfile
import unittest
from workload_paths import resolve_retained_path


class RetainedPaths(unittest.TestCase):
    def test_extracted_and_mounted_archive(self):
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d) / 'repo'
            current = repo / 'workloads/data/state'
            current.mkdir(parents=True)
            (current / 'manifest.json').write_text('active')
            archive = Path(d) / 'mounted/tmp'
            historical = archive / 'experiment/state'
            historical.mkdir(parents=True)
            (historical / 'manifest.json').write_text('historical')
            catalog = dict(legacy_roots=['/original/repository'], assets={
                'state': dict(source='copilot/tmp/experiment/state', path='workloads/data/state')})
            options = dict(repository=repo, catalog=catalog)
            old = '/original/repository/copilot/tmp/experiment/state/manifest.json'
            self.assertEqual(resolve_retained_path('state', **options), current)
            self.assertEqual(resolve_retained_path(old, **options), current / 'manifest.json')
            self.assertEqual(resolve_retained_path(old, archive_root=archive, **options),
                             historical / 'manifest.json')
            self.assertEqual(resolve_retained_path('state', archive_root=archive, **options), historical)
            with self.assertRaises(FileNotFoundError):
                resolve_retained_path(old, archive_root=Path(d) / 'missing', **options)
            self.assertFalse((repo / 'copilot').exists())

    def test_unextracted_legacy_file_and_explicit_input(self):
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d)
            archived = repo / 'archive/tmp/experiment/log'
            archived.parent.mkdir(parents=True)
            archived.write_text('log')
            options = dict(repository=repo, catalog={'assets': {}})
            self.assertEqual(resolve_retained_path('copilot/tmp/experiment/log', **options), archived)
            self.assertEqual(resolve_retained_path(archived, **options), archived)
            with self.assertRaises(FileNotFoundError):
                resolve_retained_path('copilot/tmp/missing', **options)
            self.assertFalse((repo / 'copilot').exists())


if __name__ == '__main__':
    unittest.main()
