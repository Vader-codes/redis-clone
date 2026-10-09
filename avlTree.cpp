#include <iostream>
using namespace std;

class Node
{
public:
    int data, height;
    Node *left, *right;

    Node(int val)
    {
        data = val;
        height = 1;
        left = right = NULL;
    }
};

int getheight(Node *root)
{
    return (!root) ? 0 : root->height;
}

int getbalance(Node *root)
{
    return getheight(root->left) - getheight(root->right);
}

// rightRotation
Node *rightRotation(Node *root)
{
    Node *newhead = root->left; // new root head
    Node *newRight = newhead->right;

    newhead->right = root;
    root->left = newRight;

    // update the hegight
    newhead->height = 1 + max(getheight(root->left), getheight(root->right));
    return newhead;
}
// leftRotation
Node *leftRotation(Node *root)
{
    Node *newHead = root->right;
    Node *newLeft = newHead->left;

    newHead->left = root;
    root->right = newLeft;

    newHead->height = 1 + max(getheight(root->left), getheight(root->right));
    return newHead;
}

Node *insert(Node *root, int key)
{
    // either root exists or not
    if (!root)
    {
        return new Node(key);
    }
    if (key < root->data)
    {
        root->left = insert(root->left, key);
    }
    else if (key > root->data)
    {
        root->right = insert(root->right, key);
    }
    else
        return root; // no duplicate entry

    // update height
    root->height = 1 + max(getheight(root->left), getheight(root->right));

    // check balancing
    int balance = getbalance(root);

    // left left casef (right rotation of top)
    if (balance > 1 && root->left->data > key)
    {
        return rightRotation(root);
    }
    // right right case (left rotation of top)
    else if (balance < -1 && root->right->data < key)
    {
        return leftRotation(root);
    }
    // left right case (left rotation of middle and right rotation of top)
    else if (balance > 1 && root->left->data < key)
    {
        root->left = leftRotation(root->left);
        return rightRotation(root);
    }
    // right left case (right rotation of middle and left rotation or)
    else if (balance < -1 && root->right->data < key)
    {
        root->right = rightRotation(root->right);
        return leftRotation(root);
    }
    // no unbalancing
    else
    {
        return root;
    }
}
void inorder(Node *root)
{
    if (!root)
        return;

    inorder(root->left);
    cout << root->data << " ";
    inorder(root->right);
}
int main()
{
    Node *root = NULL;

    root = insert(root, 10);
    root = insert(root, 20);
    root = insert(root, 50);
    root = insert(root, 1);
    root = insert(root, 100);
    root = insert(root, 30);
    root = insert(root, 50);
    root = insert(root, 145);
    root = insert(root, 45);
    cout << " inorder :" << endl;
    inorder(root);
    return 0;
}