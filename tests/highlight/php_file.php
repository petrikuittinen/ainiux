<?php

declare(strict_types=1);

#[Attribute(Attribute::TARGET_CLASS | Attribute::TARGET_METHOD)]
final readonly class Route
{
    public function __construct(
        public string $path,
        public array $methods = ['GET'],
    ) {}
}

enum Status: string
{
    case Draft = 'draft';
    case Published = 'published';
}

#[Route('/articles/{id}', methods: ['GET', 'HEAD'])]
final class ArticleController
{
    public private(set) Status $status = Status::Draft;

    public string $title {
        get => $this->title;
        set (string $value) {
            $this->title = trim($value);
        }
    }

    public function show(int|string $id, ?object $context = null): array
    {
        $label = $context?->user?->name ?? 'guest';
        $formatter = fn(string $value): string => strtoupper($value);
        $message = <<<HTML
Hello {$label}, article {$id}
HTML;

        return match ($this->status) {
            Status::Draft => ['preview' => $formatter($message)],
            Status::Published => ['body' => $message],
        };
    }
}

