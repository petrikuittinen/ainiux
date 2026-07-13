public final class Greeting {
    public static void main(String[] args) {
        String name = args.length > 0 ? args[0] : "world";
        System.out.println("Hello, " + name);
    }
}
